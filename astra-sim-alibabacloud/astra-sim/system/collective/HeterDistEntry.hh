#ifndef __HETERDIAS_ENTRY_HH__
#define __HETERDIAS_ENTRY_HH__

#include <string>

namespace AstraSim {

class HeterDistEntry {
    HeterDistEntry() = default;
    ~HeterDistEntry() = default;
public:
    static HeterDistEntry& getInstance() {
        static HeterDistEntry instance;
        return instance;
    }
    std::string workload;
    std::string network_topo;
    std::string network_conf;
    std::string output_dir = "./heter_dist_output";
};

}


#endif