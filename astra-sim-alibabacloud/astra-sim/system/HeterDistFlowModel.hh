#ifndef __HETERDISTFLOWMODEL_HH__
#define __HETERDISTFLOWMODEL_HH__

#include<vector>
#include<map>
#include<set>
#include <queue>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <tuple>

void init_heterdist();
std::map<int, std::vector<std::tuple<int, int, int, int, std::vector<int>, std::vector<int>, std::vector<int>>>> get_flow_model_heterdist(std::vector<int> &ranks, std::string &algo_name, size_t data_size);
#endif
