# yt-dlp extractor plugin: 斗鱼直播（Next.js 页面 / 新版签名）
#
# 内置 DouyuTVIE 仍在页面里找 `$ROOM.room_id`，斗鱼改成 Next.js 后必然失败。
# 本插件沿用 getH5Play + homeH5Enc 的流程，但：
#   · 从新页面取真实 room_id（别名 998 -> 1863767）；
#   · 用 QuickJS 或 Node.js 执行斗鱼下发的签名 JS；
#   · 返回 FLV 直链。斗鱼 HLS 的 variant 分片是相对路径且不带 token，
#     Qt/FFmpeg 不会把 variant 查询串带到分片上，实测分片 404；FLV 在
#     Qt 6.10 FFmpeg 后端可正常播放，故不再返回 HLS。
#
# 签名 JS 依赖：优先 QuickJS（随客户端分发的 qjs，约 2.5MB），没有时用
# Node.js；两者都没有才回退 PhantomJS。QuickJS 由 SYNCINE_QJS 指定，
# Node 由 SYNCINE_NODE 指定，用户也可用 DOUYU_QJS / DOUYU_NODE 覆盖。
# JS 只拿到站点脚本和一个纯 JS 的 MD5 实现，不暴露文件/进程/网络接口。

import glob
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.parse
import uuid

from yt_dlp.extractor.douyutv import DouyuTVIE
from yt_dlp.utils import (
    ExtractorError,
    UserNotLive,
    int_or_none,
    str_or_none,
    traverse_obj,
    unescapeHTML,
    url_or_none,
    urlencode_postdata,
)

__all__ = ['DouyuLiveIE']

# 纯 JS 的 MD5。QuickJS 没有 crypto 模块，而斗鱼解码后的签名函数会调用
# CryptoJS.MD5；用这份实现补上。与 hashlib.md5 的 ASCII 结果逐字节一致。
_MD5_JS = r'''
var MD5_K = [0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391];
var MD5_S = [7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];
function md5(input) {
  var bytes = [];
  for (var i = 0; i < input.length; i++) {
    var c = input.charCodeAt(i);
    if (c < 0x80) bytes.push(c);
    else if (c < 0x800) bytes.push(0xc0 | (c >> 6), 0x80 | (c & 63));
    else if (c < 0x10000) bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    else bytes.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
  }
  var bitLen = bytes.length * 8;
  bytes.push(0x80);
  while (bytes.length % 64 !== 56) bytes.push(0);
  bytes.push(bitLen & 0xff, (bitLen >>> 8) & 0xff, (bitLen >>> 16) & 0xff, (bitLen >>> 24) & 0xff, 0, 0, 0, 0);
  var a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  for (var off = 0; off < bytes.length; off += 64) {
    var M = [];
    for (var j = 0; j < 16; j++) {
      M[j] = bytes[off + j*4] | (bytes[off + j*4 + 1] << 8) | (bytes[off + j*4 + 2] << 16) | (bytes[off + j*4 + 3] << 24);
    }
    var A = a0, B = b0, C = c0, D = d0;
    for (var k = 0; k < 64; k++) {
      var F, g;
      if (k < 16) { F = (B & C) | (~B & D); g = k; }
      else if (k < 32) { F = (D & B) | (~D & C); g = (5*k + 1) % 16; }
      else if (k < 48) { F = B ^ C ^ D; g = (3*k + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7*k) % 16; }
      var tmp = D;
      D = C;
      C = B;
      var x = (A + F + MD5_K[k] + M[g]) | 0;
      B = (B + ((x << MD5_S[k]) | (x >>> (32 - MD5_S[k])))) | 0;
      A = tmp;
    }
    a0 = (a0 + A) | 0; b0 = (b0 + B) | 0; c0 = (c0 + C) | 0; d0 = (d0 + D) | 0;
  }
  function hex(v) {
    var s = '';
    for (var i = 0; i < 4; i++) {
      var b = (v >>> (i*8)) & 0xff;
      s += (b < 16 ? '0' : '') + b.toString(16);
    }
    return s;
  }
  return hex(a0) + hex(b0) + hex(c0) + hex(d0);
}
'''


class DouyuLiveIE(DouyuTVIE):
    IE_NAME = 'DouyuLive'
    IE_DESC = '斗鱼直播（Next.js 页面 / 新版签名）'
    _VALID_URL = (
        r'https?://(?:www\.)?douyu(?:tv)?\.com/'
        r'(?:topic/\w+\?rid=|(?:[^/]+/))*(?P<id>[A-Za-z0-9]+)'
    )

    # ------------------------------------------------------------------
    # Node / 签名
    # ------------------------------------------------------------------

    def _find_node(self):
        candidates = [
            os.environ.get('DOUYU_NODE'),
            os.environ.get('SYNCINE_NODE'),
            shutil.which('node'),
            shutil.which('nodejs'),
            '/usr/bin/node',
            '/usr/local/bin/node',
            '/opt/homebrew/bin/node',
        ]
        # nvm 安装在非登录 shell 的 PATH 里可能看不到，直接按版本目录找
        candidates += sorted(
            glob.glob(os.path.expanduser('~/.nvm/versions/node/*/bin/node')),
            reverse=True,
        )
        if os.name == 'nt':
            candidates += [
                os.path.expandvars(r'%ProgramFiles%\nodejs\node.exe'),
                os.path.expandvars(r'%LOCALAPPDATA%\Programs\nodejs\node.exe'),
            ]

        for candidate in candidates:
            if not candidate or not os.path.isfile(candidate):
                continue
            if os.name == 'nt' or os.access(candidate, os.X_OK):
                return candidate
        return None

    def _sign_with_node(self, node, sign_func, video_id, room_id):
        did = uuid.uuid4().hex
        tt = int(time.time())

        # 用 % 格式化而不是 f-string：sign_func 是 60KB 的站点 JS，里面
        # 大量花括号；json.dumps 只负责把它变成合法的 JS 字符串字面量。
        js = r'''
const vm = require('vm');
const crypto = require('crypto');
const sandbox = {
  CryptoJS: {
    MD5: function (message) {
      const hex = crypto.createHash('md5').update(String(message), 'utf8').digest('hex');
      return { toString: () => hex };
    }
  },
  console: { log() {}, error() {}, warn() {} }
};
vm.createContext(sandbox);
sandbox.window = sandbox;
sandbox.self = sandbox;
const source = %s + "\nub98484234(" + JSON.stringify(%s) + "," +
  JSON.stringify(%s) + "," + JSON.stringify(%s) + ")";
const result = vm.runInContext(source, sandbox, { timeout: 10000 });
process.stdout.write(String(result));
''' % (
            json.dumps(sign_func),
            json.dumps(str(room_id)),
            json.dumps(did),
            json.dumps(str(tt)),
        )

        try:
            result = subprocess.run(
                [node, '-'],
                input=js,
                text=True,
                capture_output=True,
                timeout=20,
            )
        except Exception as exc:
            raise ExtractorError(
                f'执行斗鱼签名 JS 失败（Node: {node}）', cause=exc)

        if result.returncode != 0:
            detail = (result.stderr or result.stdout or '').strip()[:500]
            raise ExtractorError(
                f'斗鱼签名 JS 失败（Node 退出码 {result.returncode}）：{detail}')

        output = (result.stdout or '').strip()
        if not output:
            raise ExtractorError('斗鱼签名 JS 没有输出')

        parsed = urllib.parse.parse_qs(output)
        sign = {key: values[0] for key, values in parsed.items() if values}
        if not sign.get('sign'):
            raise ExtractorError(f'斗鱼签名 JS 返回异常：{output[:300]}')
        self.write_debug(f'DouyuLive: Node 签名 {sign}')
        return sign

    def _find_qjs(self):
        candidates = [
            os.environ.get('DOUYU_QJS'),
            os.environ.get('SYNCINE_QJS'),
            shutil.which('qjs'),
        ]
        if os.name == 'nt':
            candidates += [
                shutil.which('qjs.exe'),
                os.path.expandvars(r'%ProgramFiles%\qjs\qjs.exe'),
            ]
        else:
            candidates += [
                '/usr/bin/qjs',
                '/usr/local/bin/qjs',
                '/opt/homebrew/bin/qjs',
            ]

        for candidate in candidates:
            if not candidate or not os.path.isfile(candidate):
                continue
            if os.name == 'nt' or os.access(candidate, os.X_OK):
                return candidate
        return None

    def _sign_with_qjs(self, qjs, sign_func, video_id, room_id):
        did = uuid.uuid4().hex
        tt = int(time.time())

        # QuickJS 没有 require/process，也没有 crypto；这里只给站点脚本
        # 一个纯 JS 的 MD5。脚本先写到临时文件，避免 60KB 参数撞命令行长度。
        script = (
            'globalThis.window = globalThis;\n'
            'globalThis.self = globalThis;\n'
            'var console = { log: function () {}, error: function () {}, warn: function () {} };\n'
            + _MD5_JS
            + '\nvar CryptoJS = { MD5: function (m) { '
              'return { toString: function () { return md5(String(m)); } }; } };\n'
            + sign_func
            + '\nprint(String(ub98484234(%s, %s, %s)));\n'
            % (
                json.dumps(str(room_id)),
                json.dumps(did),
                json.dumps(str(tt)),
            )
        )

        path = None
        try:
            with tempfile.NamedTemporaryFile(
                    'w', suffix='.js', encoding='utf-8', delete=False) as handle:
                handle.write(script)
                path = handle.name

            try:
                result = subprocess.run(
                    [qjs, path],
                    text=True,
                    capture_output=True,
                    timeout=20,
                )
            except Exception as exc:
                raise ExtractorError(
                    f'执行斗鱼签名 JS 失败（QuickJS: {qjs}）', cause=exc)

            if result.returncode != 0:
                detail = (result.stderr or result.stdout or '').strip()[:500]
                raise ExtractorError(
                    f'斗鱼签名 JS 失败（QuickJS 退出码 {result.returncode}）：{detail}')

            output = (result.stdout or '').strip()
            lines = [line for line in output.splitlines() if 'sign=' in line]
            if lines:
                output = lines[-1].strip()
            if not output:
                raise ExtractorError('斗鱼签名 JS 没有输出')

            parsed = urllib.parse.parse_qs(output)
            sign = {key: values[0] for key, values in parsed.items() if values}
            if not sign.get('sign'):
                raise ExtractorError(f'斗鱼签名 JS 返回异常：{output[:300]}')
            self.write_debug(f'DouyuLive: QuickJS 签名 {sign}')
            return sign
        finally:
            if path:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def _calc_sign(self, sign_func, video_id, a):
        # 默认优先 Node（生态最稳），没有 Node 时用随包分发的 qjs。
        # DOUYU_JS_RUNTIME=qjs/node 可强制选择，便于排查。
        runtime = (os.environ.get('DOUYU_JS_RUNTIME') or '').strip().lower()
        errors = []

        if runtime != 'qjs':
            node = self._find_node()
            if node:
                try:
                    return self._sign_with_node(node, sign_func, video_id, a)
                except Exception as exc:
                    errors.append(f'Node: {exc}')

        if runtime != 'node':
            qjs = self._find_qjs()
            if qjs:
                try:
                    return self._sign_with_qjs(qjs, sign_func, video_id, a)
                except Exception as exc:
                    errors.append(f'QuickJS: {exc}')

        if errors:
            raise ExtractorError('斗鱼签名 JS 执行失败：' + '；'.join(errors))

        self.write_debug('DouyuLive: 未找到 Node.js/qjs，回退到 PhantomJS 签名')
        try:
            return super()._calc_sign(sign_func, video_id, a)
        except Exception as exc:
            raise ExtractorError(
                '斗鱼直播签名需要 QuickJS 或 Node.js。客户端会随包提供 qjs；'
                '命令行使用时也可用环境变量 DOUYU_QJS / DOUYU_NODE 指定路径。',
                cause=exc,
            )

    # ------------------------------------------------------------------
    # 接口
    # ------------------------------------------------------------------

    def _get_sign_func(self, room_id, video_id):
        # 这个接口的 Content-Type 是 text/html，但 body 是 JSON；
        # _download_json 失败时用网页方式再解析一次。
        try:
            data = self._download_json(
                'https://www.douyu.com/swf_api/homeH5Enc',
                video_id, note='Getting signing script',
                query={'rids': room_id},
            )
        except Exception:
            text = self._download_webpage(
                'https://www.douyu.com/swf_api/homeH5Enc',
                video_id, note='Getting signing script (fallback)',
                query={'rids': room_id},
            )
            data = self._parse_json(text, video_id)

        sign_func = traverse_obj(data, ('data', f'room{room_id}', {str}))
        if not sign_func:
            raise ExtractorError('斗鱼没有下发签名脚本')
        return sign_func

    def _get_play(self, room_id, video_id, rate, sign, sign_func=None):
        for attempt in range(2):
            form = dict(sign)
            form['rate'] = rate
            response = self._download_json(
                f'https://www.douyu.com/lapi/live/getH5Play/{room_id}',
                video_id,
                note=f'Downloading livestream format {rate}',
                data=urlencode_postdata(form),
                headers={
                    'Referer': f'https://www.douyu.com/{room_id}',
                    'Origin': 'https://www.douyu.com',
                },
            )
            error = int_or_none(response.get('error'))
            if error is None or error == 0:
                return response
            if error == -9 and attempt == 0 and sign_func is not None:
                # 时间戳过期：重新取一次签名再试
                sign = self._calc_sign(sign_func, video_id, room_id)
                continue
            raise ExtractorError(
                f"斗鱼 getH5Play 返回 {error}: {response.get('msg') or '未知错误'}")

        return None

    # ------------------------------------------------------------------
    # 格式
    # ------------------------------------------------------------------

    def _formats_from_data(self, data, multirates, page_url):
        rate = int_or_none(data.get('rate'))
        rate_id = str_or_none(rate) or '0'

        rate_info = {}
        rate_index = -1
        for index, item in enumerate(multirates):
            if int_or_none(item.get('rate')) == rate:
                rate_info = item
                rate_index = index
                break

        name = str_or_none(rate_info.get('name')) or f'rate {rate_id}'
        bit = int_or_none(rate_info.get('bit')) or int_or_none(data.get('bit'))
        # bit 单调；拿不到时用 multirates 顺序（原画在前）
        quality = bit if bit else (len(multirates) - rate_index if rate_index >= 0 else 0)

        rtmp_url = url_or_none(data.get('rtmp_url'))
        rtmp_live = str_or_none(data.get('rtmp_live'))
        if not rtmp_url or not rtmp_live:
            return []

        # rtmp_url 是 ".../live"（目录），rtmp_live 是相对路径。
        # 不能用 urllib.parse.urljoin：它会把 live 当成文件名替换掉，
        # 生成 https://host/xxxx.flv（缺 /live/），CDN 返回 400。
        if rtmp_live.startswith(('http://', 'https://', '//')):
            flv_url = rtmp_live
        else:
            flv_url = rtmp_url.rstrip('/') + '/' + rtmp_live.lstrip('/')
        # 斗鱼 HLS 的媒体清单用相对分片，分片需要 variant 上的 token/wsAuth 等
        # 查询参数；Qt/FFmpeg 不会自动继承查询串，分片会 404。FLV 没有这个
        # 问题，Qt 6.10 FFmpeg 后端实测可正常播放。因此只返回 FLV。
        return [{
            'url': flv_url,
            'format_id': f'{rate_id}-flv',
            'ext': 'flv',
            'protocol': 'https',
            'format_note': name,
            'tbr': bit,
            'quality': quality,
            'http_headers': {
                'Referer': page_url,
                'Origin': 'https://www.douyu.com',
            },
        }]

    # ------------------------------------------------------------------
    # 入口
    # ------------------------------------------------------------------

    def _real_extract(self, url):
        video_id = self._match_id(url)
        webpage = self._download_webpage(url, video_id)

        # 新版页面：window.room_id = 1863767; 或 Next.js 数据里的
        # "room_id":1863767。部分房间（如 9999）只有 camelCase 的
        # `roomID: 9999`，或者整段 JSON 被转义成 `room_id\":9999`；
        # 后两种都要认，否则会误报 Unable to extract room id。
        room_id = self._search_regex(
            (
                r'window\.room_id\s*=\s*(\d+)',
                r'(?i)\\?["\']?room_?id\\?["\']?\s*[:=]\s*(\d+)',
            ),
            webpage, 'room id',
        )
        room_id = int(room_id)
        self.write_debug(f'DouyuLive: {video_id} -> room_id {room_id}')

        room = traverse_obj(
            self._download_json(
                f'https://www.douyu.com/betard/{room_id}',
                video_id,
                note='Downloading room info',
                headers={
                    'Referer': url,
                    'Origin': 'https://www.douyu.com',
                },
            ),
            'room',
        )
        if not room:
            raise ExtractorError('斗鱼没有返回房间信息')

        if int_or_none(room.get('videoLoop')) == 1:
            raise UserNotLive(
                'The channel is auto-playing VODs', video_id=room_id)
        if str_or_none(room.get('show_status')) != '1':
            raise UserNotLive(video_id=room_id)

        sign_func = self._get_sign_func(room_id, video_id)
        sign = self._calc_sign(sign_func, video_id, room_id)

        first = self._get_play(room_id, video_id, 0, sign, sign_func)
        first_data = traverse_obj(first, ('data', {dict})) or {}
        multirates = traverse_obj(first_data, ('multirates', ...)) or []

        responses = [first]
        seen_rates = set()
        first_rate = int_or_none(first_data.get('rate'))
        if first_rate is not None:
            seen_rates.add(first_rate)

        # 初始请求已经用 rate=0 试过了，不要为了 0 再请求一次
        tried_rates = {0}
        requested_rates = []
        for item in multirates:
            rate = int_or_none(item.get('rate'))
            if rate is not None and rate not in tried_rates and rate not in requested_rates:
                requested_rates.append(rate)

        for rate in requested_rates:
            if rate in seen_rates:
                continue
            response = self._get_play(room_id, video_id, rate, sign, sign_func)
            if not response:
                continue
            data = traverse_obj(response, ('data', {dict})) or {}
            returned_rate = int_or_none(data.get('rate'))
            if returned_rate is None:
                returned_rate = rate
            if returned_rate in seen_rates:
                continue
            seen_rates.add(returned_rate)
            responses.append(response)

        formats = []
        for response in responses:
            data = traverse_obj(response, ('data', {dict})) or {}
            formats.extend(self._formats_from_data(data, multirates, url))

        if not formats:
            raise ExtractorError('斗鱼没有返回可用的直播流')

        room_src = str_or_none(room.get('room_src'))
        thumbnail = None
        if room_src:
            if room_src.startswith('//'):
                thumbnail = 'https:' + room_src
            elif room_src.startswith('http'):
                thumbnail = room_src
            else:
                thumbnail = 'https://rpic.douyucdn.cn/' + room_src.lstrip('/')

        title = unescapeHTML(
            str_or_none(room.get('room_name'))
            or str_or_none(room.get('video_title'))
            or f'斗鱼直播 {room_id}')

        return {
            'id': str(room_id),
            'display_id': video_id,
            'title': title,
            'description': str_or_none(room.get('show_details')),
            'uploader': (
                str_or_none(room.get('nickname'))
                or str_or_none(room.get('owner_name'))
            ),
            'thumbnail': thumbnail,
            'is_live': True,
            'live_status': 'is_live',
            'formats': formats,
        }
