//
// Created by Victoria on 2018-03-30.
//

#include "MergerInfo.h"



MergerInfo::MergerInfo(std::map<std::string, RuntimeNode*> interface_leaf_map,
                       std::vector<ConflictItem*> conflicts_list,
                       std::vector<RuntimeNode*> service_graph) {
    this->interface_leaf_map = interface_leaf_map;
    this->conflicts_list = conflicts_list;
    this->service_graph = service_graph;
}

std::map<std::string, RuntimeNode*> MergerInfo::get_interface_leaf_map() {
    return interface_leaf_map;
};

std::vector<ConflictItem*> MergerInfo::get_conflicts_list() {
    return conflicts_list;
};

std::vector<RuntimeNode*> MergerInfo::get_service_graph() {
    return service_graph;
}
