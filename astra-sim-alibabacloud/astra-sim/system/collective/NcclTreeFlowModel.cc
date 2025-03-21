/* 
*Copyright (c) 2024, Alibaba Group;
*Licensed under the Apache License, Version 2.0 (the "License");
*you may not use this file except in compliance with the License.
*You may obtain a copy of the License at

*   http://www.apache.org/licenses/LICENSE-2.0

*Unless required by applicable law or agreed to in writing, software
*distributed under the License is distributed on an "AS IS" BASIS,
*WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*See the License for the specific language governing permissions and
*limitations under the License.
*/

#include<chrono>

#include "NcclTreeFlowModel.hh"
#include "astra-sim/system/PacketBundle.hh"
#include "astra-sim/system/RecvPacketEventHadndlerData.hh"
#include "astra-sim/system/MockNcclLog.h"
#include <iostream>


namespace AstraSim {
std::atomic<bool> NcclTreeFlowModel::g_flow_inCriticalSection(false);
NcclTreeFlowModel::NcclTreeFlowModel(
    ComType type,
    int id,
    int layer_num,
    RingTopology* ring_topology,
    uint64_t data_size,
    RingTopology::Direction direction,
    InjectionPolicy injection_policy,
    bool boost_mode,
    std::shared_ptr<MockNccl::FlowModels> ptr_flow_models,
    int treechannels)
    : Algorithm(layer_num){
  this->start_time = std::chrono::high_resolution_clock::now();
  this->end_time = std::chrono::high_resolution_clock::now();
  this->comType = type;
  this->id = id;
  this->logicalTopology = ring_topology;
  this->data_size = data_size;
  this->nodes_in_ring = ring_topology->get_nodes_in_ring();
  this->parallel_reduce = 1;
  this->toggle = false;
  this->name = Name::Ring;
  this->enabled = true;
  this->m_channels = treechannels;
  this->judge_exit_flag.store(false);
  this->judge_exit_mutex.unlock();
  this->judge_mutex.unlock();
  this->send_packets = 0;
  this->recv_packets = 0;

  pQps = new MockNccl::NcclQps();
  zero_latency_packets = new std::map<int, int>();
  non_zero_latency_packets = new std::map<int, int>();

  if (boost_mode) {
    this->enabled = ring_topology->is_enabled();
  }

  if (ring_topology->dimension == RingTopology::Dimension::Local) {
    transmition = MemBus::Transmition::Fast;
  } else {
    transmition = MemBus::Transmition::Usual;
  }

  if(ptr_flow_models){
    if(id == 0) {
      MockNcclLog* NcclLog = MockNcclLog::getInstance();
    }
    for(auto f : *ptr_flow_models) {
      flow_id_to_channel_id[f.second.flow_id] = f.second.channel_id;
      if(f.second.dest == id) {
        // free packets 记录 channel_id , src -> 总计接收的消息数量
        this->_flow_models[f.first] = f.second;
        this->free_packets[std::make_pair(f.second.channel_id,f.second.src)]++;
        recv_packets++;
      }
      if(f.second.src == id) {
        this->_flow_models[f.first] = f.second;
        if(pQps->peer_qps.count(std::make_pair(f.second.channel_id,std::make_pair(f.second.src,f.second.dest)))==0){
          pQps->peer_qps[std::make_pair(f.second.channel_id,std::make_pair(f.second.src,f.second.dest))]=1;
        }
        NcclTreeFlowModel::FlowCriticalSection cs;
        // stream_count 记录 channel_id -> 作为src发送的消息数量
        this->_stream_count[f.second.channel_id] ++;
        cs.ExitSection();
        send_packets++;
      }
    }
    // std::cout << "stream_count("<<id<<") " << this->_stream_count.size() << std::endl;
    // for(auto f : this->_stream_count) {
    //   std::cout << "stream_count("<<id<<") " << f.first << " " << f.second << std::endl;
    // }
  }
  // std::cout << "flow_model(" << id << "): ";
  // for(auto f : _flow_models) {
  //   std::cout << "(" << f.first.first << "," << f.first.second << ") ";
  // }
  // std::cout << std::endl;
  for(int channel_id = 0 ;channel_id<m_channels;channel_id++){
    assert(zero_latency_packets->find(channel_id) == zero_latency_packets->end());
    (*zero_latency_packets)[channel_id] = 0;
    assert(non_zero_latency_packets->find(channel_id) == non_zero_latency_packets->end());
    (*non_zero_latency_packets)[channel_id] = 0;
  }
  MockNccl::FlowModels::iterator tree_it;
  for(tree_it = _flow_models.begin();tree_it != _flow_models.end();tree_it++) {
    if(tree_it->second.src!=id) continue;
    int flow_id = tree_it->first.second;
    std::vector<int> valid_parent_flow_id;
    for (int parent_flow_id : tree_it->second.parent_flow_id) {
      int parent_channel_id = flow_id_to_channel_id[parent_flow_id];
      if (this->_flow_models[std::make_pair(parent_channel_id, parent_flow_id)].dest == id) {
        valid_parent_flow_id.push_back(parent_flow_id);
      }
    }
    indegree_mapping[flow_id] = valid_parent_flow_id.size();
    // std::cout << "indegree_mapping[flow_id] " << indegree_mapping[flow_id] << std::endl;
  }
  for(tree_it = _flow_models.begin();tree_it != _flow_models.end();tree_it++) {
    if(tree_it->second.src!=id) continue;
    int flow_id = tree_it->first.second;
    std::vector<int> valid_parent_flow_id;
    for (int parent_flow_id : tree_it->second.parent_flow_id) {
      int parent_channel_id = flow_id_to_channel_id[parent_flow_id];
      if (this->_flow_models[std::make_pair(parent_channel_id, parent_flow_id)].src == id) {
        valid_parent_flow_id.push_back(parent_flow_id);
      }
    }
    outdegree_mapping[flow_id] = valid_parent_flow_id.size();
    // std::cout << "outdegree_mapping[" << flow_id << "] " << outdegree_mapping[flow_id] << std::endl;
  }
  switch (type) {
    case ComType::All_Reduce:
      this->final_data_size = data_size;
      break;
    case ComType::All_Gather:
      this->final_data_size = data_size * nodes_in_ring;
      break;
    case ComType::Reduce_Scatter:
      this->final_data_size = data_size / nodes_in_ring;
      break;
    case ComType::All_to_All:
      this->final_data_size = data_size;
      break;
    default:;
  }
}

void NcclTreeFlowModel::run(EventType event, CallData* data) {
  BasicEventHandlerData* ehd = (BasicEventHandlerData*)data;
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  if (event == EventType::General) {
    int channel_id = ehd->channel_id;
    int flow_id = ehd->flow_id;
    // std::cout << "General(" << id << "): channel_id = " << channel_id << ", flow_id = " << flow_id << std::endl;
    ready(channel_id, flow_id);
  } else if (event == EventType::PacketReceived) {
    MockNcclLog* NcclLog = MockNcclLog::getInstance();
    RecvPacketEventHadndlerData* rcehd = (RecvPacketEventHadndlerData*)ehd;
    // std::cout << "PacketReceived(" << id << "): channel_id = " << rcehd->channel_id << ", flow_id = " << rcehd->flow_id << std::endl;
    AstraSim::ncclFlowTag flowTag = rcehd->flowTag;
    int received_flow_id = flowTag.current_flow_id;
    int channel_id = flowTag.channel_id;
    std::vector<int> next_flow_list = flowTag.tree_flow_list;    
    bool flow_exist = true;
    for(int i = 0; i < next_flow_list.size(); ++ i) {
      int next_flow_id = next_flow_list[i];
      int next_channel_id = flow_id_to_channel_id[next_flow_id];
      if(next_flow_id == -1 || _flow_models.count(std::make_pair(next_channel_id, next_flow_id)) != 0) {
        flow_exist = true;
      } else {
        flow_exist = false;
        break;
      }
    }
    assert(flow_exist == true);
    NcclTreeFlowModel::FlowCriticalSection cs;
    free_packets[std::make_pair(channel_id, flowTag.sender_node)]--;
    bool tag = true;
    for (int i = 0; i < m_channels; i++) {
      if (_stream_count[i] != 0) {
        tag = false;
        break;
      }
    }
    cs.ExitSection();
    if(tag) { 
      ready(channel_id, -1);
      iteratable(channel_id);
      return;
    }
    NcclLog->writeLog(NcclLogLevel::DEBUG,"PacketReceived sender_node:  %d recevier  %d current_flow id:  %d channel_id:  %d tag_id  %d free_packets  %d next_flow_list.size %d",flowTag.sender_node,flowTag.receiver_node,flowTag.current_flow_id,flowTag.channel_id,flowTag.tag_id,free_packets[std::make_pair(channel_id,flowTag.sender_node)],next_flow_list.size());
    bool flow_send = false;
    NcclLog->writeLog(NcclLogLevel::DEBUG,"next_flow_list.size %d",next_flow_list.size());
    for (int next_flow_id : next_flow_list) {
      NcclTreeFlowModel::FlowCriticalSection cs;
      // std::cout << "recv indegree_mapping[" << next_flow_id << "] " << indegree_mapping[next_flow_id] << " outdegree_mapping[" << next_flow_id << "] " << outdegree_mapping[next_flow_id] << std::endl;
      if (--indegree_mapping[next_flow_id] == 0 && outdegree_mapping[next_flow_id] == 0) {
        // std::cout << "recv issue flow_id " << next_flow_id << std::endl;
        int next_channel_id = flow_id_to_channel_id[next_flow_id];
        MockNccl::SingleFlow cur_flow = _flow_models[std::make_pair(next_channel_id, next_flow_id)];
        cs.ExitSection();
        int next_sender_node = _flow_models[std::make_pair(next_channel_id, next_flow_id)].src;
        int next_receiver_node = _flow_models[std::make_pair(next_channel_id, next_flow_id)].dest;
        auto key = std::make_pair(next_channel_id,std::make_pair(next_sender_node,next_receiver_node));
        if(pQps->peer_qps[key] == 1) {
          pQps->peer_qps[key] = 0;
          insert_packets(next_channel_id,next_flow_id);
        } else {
          pQps->peer_wating_tasks[key].push(next_flow_id);
        }
      } else {
        cs.ExitSection();
      }
    }
  } else if (event == EventType::StreamInit) {
    // 
    init_recv_ready();
    // 无依赖的任务被初始化
    for (int i = 0; i < parallel_reduce; i++) {
      for(int j = 0; j < m_channels; j ++) {
        for(const auto &flow_model : _flow_models) {
          if(flow_model.second.src!=id) continue;
          std::vector<int> parent_list = flow_model.second.parent_flow_id;
          if((parent_list.size() == 0 ) && flow_model.second.channel_id == j ) {
            auto key = std::make_pair(
                  flow_model.second.channel_id,
                  std::make_pair(flow_model.second.src, flow_model.second.dest));
            if (pQps->peer_qps[key] == 1) {
              pQps->peer_qps[key] = 0;
              insert_packets(j,flow_model.second.flow_id);
            } else {
              pQps->peer_wating_tasks[key].push(flow_model.second.flow_id);
            }
          }
        }
      }
    }

    // 有依赖的接收任务
  } else if(event == EventType::PacketSentFinshed){
    SendPacketEventHandlerData* rcehd = (SendPacketEventHandlerData*)ehd;
    AstraSim::ncclFlowTag flowTag = rcehd->flowTag;
    int sent_flow_id = flowTag.current_flow_id;
    int channel_id = flowTag.channel_id;
    // std::cout << "PacketSentFinshed(" << id << "): channel_id = " << channel_id << ", sent_flow_id = " << sent_flow_id << std::endl;
    std::vector<int> next_flow_list = flowTag.tree_flow_list;   
    NcclLog->writeLog(NcclLogLevel::DEBUG,"PacketSentFinshed src %d dst %d channel_id %d flow_id %d",flowTag.sender_node,flowTag.receiver_node,flowTag.channel_id,flowTag.current_flow_id);
    reduce(channel_id,sent_flow_id);
    for (int next_flow_id : next_flow_list) {
      NcclTreeFlowModel::FlowCriticalSection cs;
      // std::cout << "sent indegree_mapping[" << next_flow_id << "] " << indegree_mapping[next_flow_id] << " outdegree_mapping[" << next_flow_id << "] " << outdegree_mapping[next_flow_id] << std::endl;
      if (--outdegree_mapping[next_flow_id] == 0 && indegree_mapping[next_flow_id] == 0) {
        int next_channel_id = flow_id_to_channel_id[next_flow_id];
        MockNccl::SingleFlow cur_flow = _flow_models[std::make_pair(next_channel_id, next_flow_id)];
        cs.ExitSection();
        int next_sender_node = _flow_models[std::make_pair(next_channel_id, next_flow_id)].src;
        int next_receiver_node = _flow_models[std::make_pair(next_channel_id, next_flow_id)].dest;
        auto key = std::make_pair(next_channel_id,std::make_pair(next_sender_node,next_receiver_node));
        if(pQps->peer_qps[key] == 1) {
          pQps->peer_qps[key] = 0;
          insert_packets(next_channel_id,next_flow_id);
        } else {
          pQps->peer_wating_tasks[key].push(next_flow_id);
        }
      } else {
        cs.ExitSection();
      }
    }
    NcclTreeFlowModel::FlowCriticalSection cs;
    pQps->peer_qps[std::make_pair(flowTag.channel_id,std::make_pair(flowTag.sender_node,flowTag.receiver_node))]=1;
    cs.ExitSection();
    auto key = std::make_pair(flowTag.channel_id,std::make_pair(flowTag.sender_node,flowTag.receiver_node));
    if(pQps->peer_wating_tasks[key].size()>0){
      int cur_flow_id = pQps->peer_wating_tasks[key].front();
      pQps->peer_wating_tasks[key].pop();
      pQps->peer_qps[key]=0;
      insert_packets(channel_id,cur_flow_id);
    }
    // iteratable(channel_id); 
  }
}

bool NcclTreeFlowModel::init_recv_ready() {
  std::map<int,std::vector<int>> recv_ready_flows; 
  for(auto flow : _flow_models){
    if(flow.second.dest!=id)  continue;
    int cur_channel_id = flow.second.channel_id;
    int cur_flow_id = flow.second.flow_id;
    if (recv_ready_flows.count(cur_channel_id) == 0) {
      recv_ready_flows[cur_channel_id] = {cur_flow_id};
    } else { 
      recv_ready_flows[cur_channel_id].push_back(cur_flow_id);
    }
  }
  std::map<int,std::vector<int>>::iterator recv_ready_flow_it;
  for(recv_ready_flow_it = recv_ready_flows.begin();recv_ready_flow_it!=recv_ready_flows.end();recv_ready_flow_it++){
    for(int flow_id: recv_ready_flow_it->second){
      recv_ready(recv_ready_flow_it->first,flow_id);
    }
  }
  return true;
}

bool NcclTreeFlowModel::recv_ready(int channel_id, int flow_id) {
  MockNcclLog* NcclLog = MockNcclLog::getInstance();

  sim_request rcv_req;
  rcv_req.vnet = this->stream->current_queue_id;
  rcv_req.layerNum = layer_num;
  int recv_prev = _flow_models[std::make_pair(channel_id, flow_id)].src;

  RecvPacketEventHadndlerData* ehd = new RecvPacketEventHadndlerData(
        stream,
        stream->owner->id,
        EventType::PacketReceived,
        recv_prev,
        1);
  
  ehd->flow_id = flow_id;
  ehd->channel_id = channel_id;
  ehd->flowTag.child_flow_id = -1;
  ehd->flowTag.current_flow_id = flow_id;
  ehd->flowTag.channel_id = channel_id;
  ehd->flowTag.tag_id = flow_id;
  ehd->flowTag.tree_flow_list = this->_flow_models[std::make_pair(channel_id, flow_id)].child_flow_id;
  stream->owner->front_end_sim_recv(
        0,
        Sys::dummy_data,
        _flow_models[std::make_pair(channel_id, flow_id)].flow_size,
        UINT8,
        recv_prev,
        channel_id,
        &rcv_req,
        &Sys::handleEvent,
        ehd);
  return true;
}

void NcclTreeFlowModel::release_packets(int channel_id, int flow_id, uint64_t message_size) {
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  if (NPU_to_MA == true) {
    (new PacketBundle(
         stream->owner,
         stream,
         {},
         processed,
         send_back,
         message_size,
         transmition,
         channel_id,
         flow_id))
        ->send_to_MA();
  } else {
    (new PacketBundle(
         stream->owner,
         stream,
         {},
         processed,
         send_back,
         message_size,
         transmition,
         channel_id,
         flow_id))
        ->send_to_NPU();
  }
  NcclLog->writeLog(NcclLogLevel::DEBUG,"id:  %d finish release_packets",id);
}

void NcclTreeFlowModel::process_stream_count(int channel_id) {
  MockNcclLog*NcclLog = MockNcclLog::getInstance();
  NcclTreeFlowModel::FlowCriticalSection cs;
  if (_stream_count[channel_id] > 0) {
    _stream_count[channel_id]--;
  }
  NcclLog->writeLog(NcclLogLevel::DEBUG,"NcclTreeFlowModel::process_stream_count channel_id %d _stream_count %d",channel_id,_stream_count[channel_id]);
  if (_stream_count[channel_id] == 0 && stream->state != StreamState::Dead) {
    // std::cout << "stream_count("<<id<<") " << _stream_count[channel_id] << std::endl;
    stream->changeState(StreamState::Zombie);
  }
  cs.ExitSection();
}

void NcclTreeFlowModel::reduce(int channel_id, int flow_id) {
  process_stream_count(channel_id);
  if(!packets[std::make_pair(channel_id, flow_id)].empty()){
    packets[std::make_pair(channel_id, flow_id)].pop_front();
  }
}

bool NcclTreeFlowModel::iteratable(int channel_id) {
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  bool all_channel_finished = true, all_packets_freed = true;
  NcclTreeFlowModel::FlowCriticalSection cs;
  for(int i = 0; i < m_channels; ++ i) {
    if(_stream_count.count(i) != 0 && _stream_count[i] != 0) all_channel_finished = false;
  }
  for (auto it = free_packets.begin(); it != free_packets.end(); it++) {
    if (it->second != 0) {
      all_packets_freed = false;
      break;
    }
  }
  cs.ExitSection();
  if (all_channel_finished == true &&
      all_packets_freed == true) {
    exit();
    return false;
  }
  return true;
}

void NcclTreeFlowModel::insert_packets(int channel_id, int flow_id) {
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  assert(channel_id < m_channels);
  if (!enabled) {
    return;
  }
  assert(_flow_models.count(std::make_pair(channel_id, flow_id)) != 0);

  MockNccl::SingleFlow f = _flow_models[std::make_pair(channel_id, flow_id)];
  assert(zero_latency_packets->count(channel_id) != 0 && non_zero_latency_packets->count(channel_id) != 0);
  if ((*zero_latency_packets)[channel_id] == 0 && (*non_zero_latency_packets)[channel_id] == 0) {
    (*zero_latency_packets)[channel_id] = parallel_reduce * 1;
    (*non_zero_latency_packets)[channel_id] = get_non_zero_latency_packets();
    toggle = !toggle;
  }
  int current_receiver = f.dest;
  int current_sender = f.src;
  if ((*zero_latency_packets)[channel_id] > 0) {
    NcclLog->writeLog(NcclLogLevel::DEBUG,"id:  %d (*zero_latency_packets)[channel_id] > 0",id);
    uint64_t message_size = f.flow_size;
    packets[std::make_pair(channel_id, flow_id)].push_back(MyPacket(
        stream->current_queue_id,
        current_sender, 
        current_receiver,
        message_size,
        channel_id,
        flow_id));
    packets[std::make_pair(channel_id, flow_id)].back().set_flow_id(flow_id);
    packets[std::make_pair(channel_id, flow_id)].back().sender = nullptr;
    processed = false;
    send_back = false;
    NPU_to_MA = true;
    release_packets(channel_id, flow_id, message_size);
    (*zero_latency_packets)[channel_id]--;
    NcclLog->writeLog(NcclLogLevel::DEBUG,"id:  %d (*zero_latency_packets)[channel_id] : %d ",id,(*zero_latency_packets)[channel_id]);
    return;
  } else if ((*non_zero_latency_packets)[channel_id] > 0) {
    NcclLog->writeLog(NcclLogLevel::DEBUG,"id:  %d (*non_zero_latency_packets)[channel_id] > 0",id);
    uint64_t message_size = f.flow_size;
    packets[std::make_pair(channel_id, flow_id)].push_back(MyPacket(
        stream->current_queue_id,
        current_sender,  
        current_receiver,
        message_size,
        channel_id,
        flow_id)); 
    packets[std::make_pair(channel_id, flow_id)].back().set_flow_id(flow_id);
    packets[std::make_pair(channel_id, flow_id)].back().sender = nullptr;
    if (comType == ComType::Reduce_Scatter ||
        (comType == ComType::All_Reduce && toggle)) {
      processed = true;
    } else {
      processed = false;
    }
    if ((*non_zero_latency_packets)[channel_id] <= parallel_reduce * 1) {
      send_back = false;
    } else {
      send_back = true;
    }
    NPU_to_MA = false;
    release_packets(channel_id, flow_id, message_size);
    (*non_zero_latency_packets)[channel_id]--;
    NcclLog->writeLog(NcclLogLevel::DEBUG,"id:  %d (*non_zero_latency_packets)[channel_id] : %d ",id,(*non_zero_latency_packets)[channel_id]);
    return;
  }
  Sys::sys_panic("should not inject nothing!");
}

bool NcclTreeFlowModel::ready(int channel_id, int flow_id) {
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  MyPacket packet;
  {
    if (stream->state == StreamState::Created ||
        stream->state == StreamState::Ready) {
      stream->changeState(StreamState::Executing);
    }
    if (!enabled || packets[std::make_pair(channel_id, flow_id)].size() == 0 || _stream_count[channel_id] == 0) {
      NcclLog->writeLog(NcclLogLevel::DEBUG,"NcclTreeFlowModel not ready!");
      return false;
    }
    packet = packets[std::make_pair(channel_id, flow_id)].front();
  }
  std::vector<int> recv_prevs;
  recv_prevs = _flow_models[std::make_pair(channel_id, flow_id)].prev;
  std::vector<int> recv_prev_flow_ids;
  recv_prev_flow_ids = _flow_models[std::make_pair(channel_id, flow_id)].parent_flow_id;
  for (int i = 0; i < recv_prevs.size(); i++) {
    int recv_prev = recv_prevs[i];
    int recv_prev_flow_id = recv_prev_flow_ids[i];
    sim_request rcv_req;
    rcv_req.vnet = this->stream->current_queue_id;
    rcv_req.layerNum = layer_num;
    rcv_req.reqCount = packet.msg_size;
    rcv_req.tag = recv_prev_flow_id;
    RecvPacketEventHadndlerData* ehd = new RecvPacketEventHadndlerData(
        stream,
        stream->owner->id,
        EventType::PacketReceived,
        packet.preferred_vnet,
        packet.stream_num);
    ehd->flowTag.child_flow_id = -1;
    ehd->flowTag.current_flow_id = recv_prev_flow_id;
    auto flow_model = this->_flow_models[std::make_pair(channel_id,flow_id)];
    ehd->flowTag.tag_id = recv_prev_flow_id;
    ehd->flowTag.channel_id = packet.channel_id;
    ehd->flowTag.tree_flow_list = this->_flow_models[std::make_pair(channel_id, recv_prev_flow_id)].child_flow_id;
    if (free_packets[std::make_pair(channel_id, recv_prev)] > 0) {
      stream->owner->front_end_sim_recv(
          0,
          Sys::dummy_data,
          rcv_req.reqCount,
          UINT8,
          recv_prev,
          rcv_req.tag,
          &rcv_req,
          &Sys::handleEvent,
          ehd);
    }
  }
  sim_request snd_req;
  snd_req.srcRank = id;
  snd_req.dstRank = packet.preferred_dest;
  snd_req.tag = this->_flow_models[std::make_pair(channel_id, flow_id)].flow_id;
  snd_req.reqType = UINT8;
  snd_req.vnet = this->stream->current_queue_id;
  snd_req.layerNum = layer_num;
  snd_req.reqCount = packet.msg_size;
  MockNccl::SingleFlow flow_model =
      this->_flow_models[std::make_pair(channel_id, flow_id)];
  snd_req.flowTag.tag_id = flow_id;
  snd_req.flowTag.channel_id = channel_id;
  snd_req.flowTag.flow_size = flow_model.flow_size;
  snd_req.flowTag.current_flow_id = flow_id;
  snd_req.flowTag.chunk_id = flow_model.chunk_id;
  snd_req.flowTag.child_flow_id = -1;
  snd_req.flowTag.tree_flow_list = this->_flow_models[std::make_pair(channel_id, flow_id)].child_flow_id;
  snd_req.flowTag.sender_node = id;
  snd_req.flowTag.receiver_node = packet.preferred_dest;
  snd_req.flowTag.pQps = this->pQps;
  if (this->comType == ComType::All_Reduce_NVLS)
    snd_req.flowTag.nvls_on = true;
  else
    snd_req.flowTag.nvls_on = false;
  SendPacketEventHandlerData* send_ehd = new SendPacketEventHandlerData(
      stream,
      id,
      packet.preferred_dest,
      flow_id,
      EventType::PacketSentFinshed);
  stream->owner->front_end_sim_send(
      0,
      Sys::dummy_data,
      snd_req.reqCount,
      UINT8,
      packet.preferred_dest,
      snd_req.flowTag.tag_id,
      &snd_req,
      &Sys::handleEvent,
      send_ehd);
  return true;
}

void NcclTreeFlowModel::exit() {
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  for(std::pair<std::pair<int, int>, std::list<MyPacket>> packet: packets) {
  if(packet.second.size() != 0)
    packet.second.clear();
  }
  stream->owner->proceed_to_next_vnet_baseline((StreamBaseline*)stream);
  NcclLog->writeLog(NcclLogLevel::DEBUG,"NcclTreeFlowModel exit");
  return;
}

int NcclTreeFlowModel::get_non_zero_latency_packets() {
  return (nodes_in_ring - 1) * parallel_reduce * 1;
}
} // namespace AstraSim