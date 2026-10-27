//
// 文件传输的两条 DataChannel。
//
// 控制消息（JSON）走 Control：reliable + unordered。它与文件块分开后，
// 不再被数据通道的重传队列堵住 —— 取消、校验、心跳都能及时到达。
// 文件块走 Data：reliable + ordered，接收端按到达顺序写文件。
//

#ifndef SYNCINE_TRANSFERCHANNELS_H
#define SYNCINE_TRANSFERCHANNELS_H

namespace TransferChannel {

// 应用层标识，用在信号与 WebrtcManager 的接口里
inline constexpr char ControlId[] = "ctl";
inline constexpr char DataId[] = "data";

// SCTP 通道名，协商时对端按它区分两条通道
inline constexpr char ControlLabel[] = "syncine-file-ctl";
inline constexpr char DataLabel[] = "syncine-file-data";

} // namespace TransferChannel

#endif //SYNCINE_TRANSFERCHANNELS_H
