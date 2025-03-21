#include "HeterDistFlowModel.hh"
#include "collective/HeterDistEntry.hh"
#include <array>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <cstdint>
#include <tuple>
#include <map>
#include <vector>
#include <algorithm>

uint64_t uniform_ranks_hash(const std::vector<int> &ranks, const std::string &comm_type, uint64_t size);

class HeterDistFlowModel {
HeterDistFlowModel() {
    executeGenerateUtilCommand();
};
~HeterDistFlowModel() = default;
public:
    static HeterDistFlowModel &getInstance() {
        static HeterDistFlowModel instance;
        return instance;
    }

    bool executeGenerateUtilCommand() {
        AstraSim::HeterDistEntry &entry = AstraSim::HeterDistEntry::getInstance();
        std::cout << "Executing generate_util command..." << std::endl;
        std::string cmd_str = "./generate_util -t " + entry.network_topo + " -a " + entry.workload + " -o " + entry.output_dir + " -p simai-loader,greedy-gen,milp-dag-scheduler,p2p-exporter";
        const char* cmd = cmd_str.c_str();
        std::array<char, 128> buffer;
        std::string result;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) {
            std::cerr << "popen() failed!" << std::endl;
            return false;
        }
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }
        int status = pclose(pipe);
        if (status == 0) {
            std::cout << "Command executed successfully" << std::endl;
            std::cout << result << std::endl;
        } else {
            std::cerr << "Command execution failed with code: " << status << std::endl;
            return false;
        }
        return true;
    }
};

void init_heterdist() {
    AstraSim::HeterDistEntry &entry = AstraSim::HeterDistEntry::getInstance();
    HeterDistFlowModel &flow_model = HeterDistFlowModel::getInstance();
}

std::map<int, std::vector<std::tuple<int, int, int, int, std::vector<int>, std::vector<int>, std::vector<int>>>> get_flow_model_heterdist(std::vector<int> &ranks, std::string &algo_name, size_t data_size) {
    AstraSim::HeterDistEntry &entry = AstraSim::HeterDistEntry::getInstance();
    uint64_t hash_value = uniform_ranks_hash(ranks, algo_name, data_size);

    HeterDistFlowModel &flow_model = HeterDistFlowModel::getInstance();

    std::string task_file = entry.output_dir + "/" + std::to_string(hash_value) + "/tasks.txt";

    // 结果映射: rank -> vector<tuple<task_id, src, dst, chunk_id, prev_nodes, prev_dep_tasks, post_dep_tasks>>
    std::map<int, std::vector<std::tuple<int, int, int, int, std::vector<int>, std::vector<int>, std::vector<int>>>> result;
    
    // 打开任务文件
    std::ifstream file(task_file);
    if (!file.is_open()) {
        std::cerr << "无法打开任务文件: " << task_file << std::endl;
        return result;
    }
    
    // 读取任务数量
    int num_tasks;
    file >> num_tasks;
    
    // 存储所有任务信息: task_id -> (src, dst, chunk_id)
    std::map<int, std::tuple<int, int, int>> tasks;
    
    // 读取每个任务的基本信息
    for (int i = 0; i < num_tasks; i++) {
        int task_id, src, dst, chunk_id;
        file >> task_id >> src >> dst >> chunk_id;
        // std::cout << "task_id = " << task_id << ", src = " << src << ", dst = " << dst << ", chunk_id = " << chunk_id << std::endl;
        tasks[task_id] = std::make_tuple(src, dst, chunk_id);
    }
    
    // 读取依赖数量
    int dep_num;
    file >> dep_num;
    
    // 存储任务依赖关系: task_id -> vector<prev_task_id>
    std::map<int, std::vector<int>> task_deps;
    
    // 存储任务后续依赖关系: task_id -> vector<post_task_id>
    std::map<int, std::vector<int>> task_post_deps;
    
    // 读取依赖关系
    for (int i = 0; i < dep_num; i++) {
        int cur_task_id, prev_dep_task_id;
        file >> cur_task_id >> prev_dep_task_id;
        if (task_deps.find(cur_task_id) == task_deps.end()) {
            task_deps[cur_task_id] = std::vector<int>();
        }
        task_deps[cur_task_id].push_back(prev_dep_task_id);
        if (task_post_deps.find(prev_dep_task_id) == task_post_deps.end()) {
            task_post_deps[prev_dep_task_id] = std::vector<int>();
        }
        task_post_deps[prev_dep_task_id].push_back(cur_task_id);
    }
    
    std::map<int, std::vector<int>> task_prev_nodes;
    for (const auto& [task_id, prev_tasks] : task_deps) {
        int src = std::get<0>(tasks[task_id]);
        for (int prev_task_id : prev_tasks) {
            if (std::get<1>(tasks[prev_task_id]) == src) {
                int prev_src = std::get<0>(tasks[prev_task_id]);
                task_prev_nodes[task_id].push_back(prev_src);
            }
        }
    }
    
    for (const auto& [task_id, task_info] : tasks) {
        int src = std::get<0>(task_info);
        int dst = std::get<1>(task_info);
        int chunk_id = std::get<2>(task_info);
        
        // 只包含src或dst是当前rank的任务
        
        std::tuple<int, int, int, int, std::vector<int>, std::vector<int>, std::vector<int>> task_tuple = 
        std::make_tuple(
                        task_id, 
                        src, 
                        dst, 
                        chunk_id, 
                        task_prev_nodes[task_id], 
                        task_deps[task_id], 
                        task_post_deps[task_id]
                    );
                
        result[src].push_back(task_tuple);
        result[dst].push_back(task_tuple);
        
    }
    std::cout << "result: " << result.size() << std::endl;
    return result;
}

uint64_t xx_hash(const std::vector<int> &ranks, int seed_value = 0) {
    const uint64_t PRIME1 = 11400714785074694791ULL;
    const uint64_t PRIME2 = 14029467366897019727ULL;
    const uint64_t PRIME3 = 1609587929392839161ULL;
    const uint64_t PRIME4 = 9650029242287828579ULL;
    const uint64_t PRIME5 = 2870177450012600261ULL;
    
    uint64_t seed = static_cast<uint64_t>(seed_value);
    uint64_t h64;
    
    if (ranks.empty()) {
        return PRIME5 + seed;
    }
    
    size_t len = ranks.size() * sizeof(int);
    const unsigned char* data = reinterpret_cast<const unsigned char*>(ranks.data());
    const unsigned char* const end = data + len;
    
    if (len >= 32) {
        const unsigned char* const limit = end - 32;
        uint64_t v1 = seed + PRIME1 + PRIME2;
        uint64_t v2 = seed + PRIME2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - PRIME1;
        
        do {
            v1 += *reinterpret_cast<const uint64_t*>(data) * PRIME2;
            v1 = (v1 << 31) | (v1 >> 33);
            v1 *= PRIME1;
            data += 8;
            
            v2 += *reinterpret_cast<const uint64_t*>(data) * PRIME2;
            v2 = (v2 << 31) | (v2 >> 33);
            v2 *= PRIME1;
            data += 8;
            
            v3 += *reinterpret_cast<const uint64_t*>(data) * PRIME2;
            v3 = (v3 << 31) | (v3 >> 33);
            v3 *= PRIME1;
            data += 8;
            
            v4 += *reinterpret_cast<const uint64_t*>(data) * PRIME2;
            v4 = (v4 << 31) | (v4 >> 33);
            v4 *= PRIME1;
            data += 8;
        } while (data <= limit);
        
        h64 = ((v1 << 1) | (v1 >> 63)) + 
              ((v2 << 7) | (v2 >> 57)) + 
              ((v3 << 12) | (v3 >> 52)) + 
              ((v4 << 18) | (v4 >> 46));
        
        v1 *= PRIME2;
        v1 = (v1 << 31) | (v1 >> 33);
        v1 *= PRIME1;
        h64 ^= v1;
        h64 = h64 * PRIME1 + PRIME4;
        
        v2 *= PRIME2;
        v2 = (v2 << 31) | (v2 >> 33);
        v2 *= PRIME1;
        h64 ^= v2;
        h64 = h64 * PRIME1 + PRIME4;
        
        v3 *= PRIME2;
        v3 = (v3 << 31) | (v3 >> 33);
        v3 *= PRIME1;
        h64 ^= v3;
        h64 = h64 * PRIME1 + PRIME4;
        
        v4 *= PRIME2;
        v4 = (v4 << 31) | (v4 >> 33);
        v4 *= PRIME1;
        h64 ^= v4;
        h64 = h64 * PRIME1 + PRIME4;
    } else {
        h64 = seed + PRIME5;
    }
    
    h64 += static_cast<uint64_t>(len);
    
    while (data + 8 <= end) {
        uint64_t k1 = *reinterpret_cast<const uint64_t*>(data);
        k1 *= PRIME2;
        k1 = (k1 << 31) | (k1 >> 33);
        k1 *= PRIME1;
        h64 ^= k1;
        h64 = ((h64 << 27) | (h64 >> 37)) * PRIME1 + PRIME4;
        data += 8;
    }
    
    if (data + 4 <= end) {
        h64 ^= static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(data)) * PRIME1;
        h64 = ((h64 << 23) | (h64 >> 41)) * PRIME2 + PRIME3;
        data += 4;
    }
    
    while (data < end) {
        h64 ^= static_cast<uint64_t>(*data) * PRIME5;
        h64 = ((h64 << 11) | (h64 >> 53)) * PRIME1;
        data++;
    }
    
    h64 ^= h64 >> 33;
    h64 *= PRIME2;
    h64 ^= h64 >> 29;
    h64 *= PRIME3;
    h64 ^= h64 >> 32;
    
    return h64;
}

uint64_t fast_hash(const std::vector<int> &ranks, int seed = 0) {
    if (ranks.size() <= 4) {
        const uint64_t PRIME1 = 11400714785074694791ULL;
        const uint64_t PRIME2 = 14029467366897019727ULL;
        const uint64_t PRIME3 = 1609587929392839161ULL;
        
        uint64_t hash = static_cast<uint64_t>(seed);
        for (int rank : ranks) {
            hash ^= static_cast<uint64_t>(rank) * PRIME2;
            hash = ((hash << 13) | (hash >> 51)) * PRIME1;
        }
        
        hash ^= hash >> 33;
        hash *= PRIME2;
        hash ^= hash >> 29;
        hash *= PRIME3;
        
        return hash;
    } else {
        return xx_hash(ranks, seed);
    }
}

int get_str_hash(const std::string &str) {
    unsigned long hash = 5381;
    for (char c : str) {
        hash = ((hash << 5) + hash) + c;
    }
    return static_cast<int>(hash % 2147483647); 
}

uint64_t uniform_ranks_hash(const std::vector<int> &ranks, const std::string &comm_type, uint64_t size) {
    int hash_value = get_str_hash(comm_type);
    std::vector<int> ranks_hash = ranks;
    ranks_hash.push_back(size);
    ranks_hash.push_back(hash_value);
    return fast_hash(ranks_hash);
}
