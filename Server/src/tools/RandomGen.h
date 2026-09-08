//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_RANDOMGEN_H
#define SERVER_RANDOMGEN_H

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

class RandomGen {
public:
    static std::string generateUUID() {
        static boost::uuids::random_generator gen;
        boost::uuids::uuid uuid = gen();
        return to_string(uuid);
    }
};
#endif //SERVER_RANDOMGEN_H
