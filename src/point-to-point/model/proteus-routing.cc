#include "ns3/proteus-routing.h"

#include <iostream>
#include <random>

#include "ns3/log.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ppp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/random-variable.h"


NS_LOG_COMPONENT_DEFINE("ProteusRouing");

namespace ns3 {

    ProteusRouting::ProteusRouting() {
		m_isToR = false;
		m_switch_id = (uint32_t)-1;
	
		// set constants
		m_dreTime = Time(MicroSeconds(200));
		m_alpha = 0.2;
		m_onewayRttLow = NanoSeconds(4160*1.3);
	}
    ProteusRouting::~ProteusRouting() {}

	TypeId ProteusRouting::GetTypeId(void) {
		static TypeId tid =
			TypeId("ns3::ProteusRouting")
				.SetParent<Object>()
				.AddConstructor<ProteusRouting>();
	
		return tid;
	}

	void ProteusRouting::RouteInput(Ptr<Packet> p, CustomHeader &ch) {
		// DoSwitchSendToDev(p, ch);
        // return;

        Time now = Simulator::Now();

		// 由于probe包不需要到host 同时为了测正常udp包的rtt 所以不能像conweave用0xFD等ACK协议号 这会让包走优先级更高于udp的队列
		// 所以 这里若是有probeTag 则表明是探测包 拦截下来 不要再往host传了
		ProteusProbeTag probeTag;
		bool foundProteusProbeTag = p->PeekPacketTag(probeTag);
		
		// get ProteusTag
		ProteusTag proteusTag;
		bool foundProteusTag = p->PeekPacketTag(proteusTag);

		/*************  一些必做的处理  *************/
		uint32_t srcToRId = Settings::hostIp2SwitchId[ch.sip];
    	uint32_t dstToRId = Settings::hostIp2SwitchId[ch.dip];

		// 只处理UDP包(RoCEv2) or "do normal routing (only one path)"
		// 探测包 不转发到host
		if ((ch.l3Prot != 0x11 || srcToRId == dstToRId) && !foundProteusProbeTag) {
			DoSwitchSendToDev(p, ch);
			return;
		}

		// 探测包只在ToR之间传 sip dip 都是ToR的Ip 
		// 用 hostIpSwitchId 获取不到相应SwitchId
		// 故用以下方式(其实可以统一用下面这个方式的吧？)
		if(foundProteusProbeTag) {
			srcToRId = Settings::ip_to_node_id(Ipv4Address(ch.sip));
			dstToRId = Settings::ip_to_node_id(Ipv4Address(ch.dip));
		}




		if (m_isToR) {
			// 启动周期性主动探测  只ToR需要探测
			if (!m_probeEvent.IsRunning()) {
				m_probeEvent = Simulator::ScheduleNow(&ProteusRouting::ProbeEvent, this);
			}

			// 监控链路利用率 只ToR需要检测
			if (!m_dreEvent.IsRunning()) {
				m_dreEvent = Simulator::Schedule(m_dreTime, &ProteusRouting::DreEvent, this);
			}
			

			if (m_switch_id == srcToRId) {  // 源ToR
				// 有dstToR路径信息 进行捎带
				if (m_proteusFbPathPtr.find(dstToRId) == m_proteusFbPathPtr.end()) {
					m_proteusFbPathPtr[dstToRId] = 0;
				}
				
				std::vector<std::tuple<uint32_t, bool, uint64_t>>::iterator it;
				if (m_proteusFbPathTable.find(dstToRId) != m_proteusFbPathTable.end()) {  // findout 就有至少一条路径信息
					it = m_proteusFbPathTable.find(dstToRId)->second.begin();
				
					if (it != m_proteusFbPathTable.find(dstToRId)->second.end()) {
						it += m_proteusFbPathPtr[dstToRId];
				
						bool found = false;
						for (uint32_t count = 0; count < m_proteusFbPathTable.find(dstToRId)->second.size(); count++) {
							if (std::get<1>(*it)) {  // 标志为1 表示路径信息更新了 需要捎带
								m_proteusFbPathPtr[dstToRId]++;  // 下一次从下一个位置开始找
								if (m_proteusFbPathPtr[dstToRId] >= m_proteusFbPathTable.find(dstToRId)->second.size()) {
									m_proteusFbPathPtr[dstToRId] = 0;
								}

								std::get<1>(*it) = false;
				
								found = true;
								break;
							}
				
							it++;
							m_proteusFbPathPtr[dstToRId]++;
							if (it == m_proteusFbPathTable.find(dstToRId)->second.end()) {
								it = m_proteusFbPathTable.find(dstToRId)->second.begin();
								m_proteusFbPathPtr[dstToRId] = 0;
							}
						}
				
						if (found) {
							// 找到就捎带
							ProteusFeedbackTag proteusFbTag;
							proteusFbTag.SetFbPathId(std::get<0>(*it));
							proteusFbTag.SetPathRtt(std::get<2>(*it));

							uint32_t inport = m_pathId2Port[proteusFbTag.GetFbPahtId()];
							double utilization = GetInPortUtil(inport);
							proteusFbTag.SetPathUtilization(utilization);

							p->AddPacketTag(proteusFbTag);
						}
					}
				}

				std::set<uint32_t> pathSet = m_proteusRoutingTable[dstToRId];
				
				// debug：随机选一条path
				std::srand(static_cast<unsigned int>(ch.sip | (ch.dip << 16)));
				uint32_t selectedPath = *(std::next(pathSet.begin(), rand() % pathSet.size()));
				std::vector<uint32_t> selectedPathSet;

				uint64_t qpkey = ((uint64_t)ch.dip << 32) | ((uint64_t)ch.udp.sport << 16) | (uint64_t)ch.udp.pg | (uint64_t)ch.udp.dport;
				if (m_proteusFlowTable.find(qpkey) != m_proteusFlowTable.end()) {
					selectedPath = m_proteusFlowTable[qpkey];
				} else {				
					selectedPathSet = GetPahtSet(dstToRId, 4);
					selectedPath = GetFinalPath(dstToRId, selectedPathSet, ch);
					m_proteusFlowTable[qpkey] = selectedPath;
				}

				proteusTag.SetPathId(selectedPath);
				proteusTag.SetHopCount(0);

				p->AddPacketTag(proteusTag);

				uint32_t outport = GetOutPortFromPath(selectedPath, 0);

				DoSwitchSend(p, ch, outport, 3);
			} else {  // 目的ToR
				// uint64_t timestampTx = proteusTag.GetTimestampTx();
				// std::cout << "one-way rtt "
				// 			<< "size=" << p->GetSize() << " "
				// 			<< srcToRId << "-->" << dstToRId << ": "
				// 			<< now.GetNanoSeconds() - NanoSeconds(timestampTx)
				// 			<< std::endl;

				// 收到数据包 先更新本地端口Dre
				FlowIdTag inport;
				p->PeekPacketTag(inport);
				m_pathId2Port[proteusTag.GetPathId()] = inport.GetFlowId();

				UpdateLocalDre(p, ch, inport.GetFlowId());


				ProteusFeedbackTag proteusFbTag;
				bool foundProteusFbTag = p->PeekPacketTag(proteusFbTag);

				// 若是收到反馈的路径信息
				if (foundProteusFbTag) {
					uint32_t fbPathId = proteusFbTag.GetFbPahtId();
					struct proteusPathInfo pathInfo = {proteusFbTag.GetPathRtt(), proteusFbTag.GetUtilization()};
					// 更新路径信息
					m_proteusPathInfoTable[srcToRId][fbPathId] = pathInfo;
					// 取下路径信息Tag
					p->RemovePacketTag(proteusFbTag);
				}


				// 若是探测包 则计算rtt 且不要往后传了！
				if (foundProteusProbeTag) {
					uint32_t probeToRId = srcToRId;
					uint32_t probePathId = proteusTag.GetPathId();
					uint64_t oneway_rtt = now.GetNanoSeconds() - probeTag.GetTimestampTx();
					// std::cout << "oneway_rtt: " << oneway_rtt <<std::endl;
					// 记录 path和rtt在表中  设置update为1  作为源ToR时round-robin捎带回去 捎带时将update设为0 就不用再带了
					// updateFbPath();

					if (m_proteusFbPathTable.find(probeToRId) == m_proteusFbPathTable.end()) {
						m_proteusFbPathTable[probeToRId];
					}				
					
					std::vector<std::tuple<uint32_t, bool, uint64_t>>::iterator it;
					for (it = m_proteusFbPathTable.find(probeToRId)->second.begin();
							it !=  m_proteusFbPathTable.find(probeToRId)->second.end();
							it++) {
						if (std::get<0>(*it) == probePathId) {
							std::get<1>(*it) = true;
							std::get<2>(*it) = oneway_rtt;
							break;
						}
					}
					if (it == m_proteusFbPathTable.find(probeToRId)->second.end()) {  // 没有找到 创建
						m_proteusFbPathTable.find(probeToRId)->second.push_back(
							std::make_tuple(probePathId, true, oneway_rtt));
					}
				} else {  // 正常处理流程
					p->RemovePacketTag(proteusTag);
					DoSwitchSendToDev(p, ch);
				}
			}
		} else {  // 非ToR
			uint32_t hopCount = proteusTag.GetHopCount() + 1;
			proteusTag.SetHopCount(hopCount);
			
			ProteusTag temp_tag;
			p->RemovePacketTag(temp_tag);
			p->AddPacketTag(proteusTag);
			
			uint32_t outport = GetOutPortFromPath(proteusTag.GetPathId(), hopCount);

			DoSwitchSend(p, ch, outport, 3);
		}

		return;
	}

	/*************  数据包Send回调函数  *************/
	void ProteusRouting::SetSwitchSendCallback(SwitchSendCallback switchSendCallback) {
		m_switchSendCallback = switchSendCallback;
	}
	
	void ProteusRouting::SetSwitchSendToDevCallback(SwitchSendToDevCallback switchSendToDevCallback) {
		m_switchSendToDevCallback = switchSendToDevCallback;
	}

	void ProteusRouting::DoSwitchSend(Ptr<Packet> p, CustomHeader& ch, uint32_t outDev, uint32_t qIndex) {
		m_switchSendCallback(p, ch, outDev, qIndex);
	}

	void ProteusRouting::DoSwitchSendToDev(Ptr<Packet> p, CustomHeader& ch) {
		m_switchSendToDevCallback(p, ch);
	}

	/*************  获取port队列长度 回调函数  *************/
	// 捆绑 SwitchNode::CalculateInterfaceLoad()
	void ProteusRouting::SetGetInterfaceLoadCallback(GetInterfaceLoadCallback callbackFunction) {
		m_getInterfaceLoadCallback = callbackFunction;
	}

	uint32_t ProteusRouting::GetInterfaceLoad(uint32_t interface) {
		m_getInterfaceLoadCallback(interface);
	}

	/*************  获取port暂停状态 回调函数  *************/
	// 捆绑 SwitchNode::GetInterfacePauseStatus()
	void ProteusRouting::SetGetInterfacePauseCallback(GetInterfacePauseCallback callbackFunction) {
		m_getInterfacePauseCallback = callbackFunction;
	}

    bool* ProteusRouting::GetInterfacePause(uint32_t interface) {
		m_getInterfacePauseCallback(interface);
	}

	/*************  交换机参数设置  *************/
	void ProteusRouting::SetSwitchInfo(bool isToR, uint32_t switch_id) {
		m_isToR = isToR;
    	m_switch_id = switch_id;
	}

	void ProteusRouting::SetConstants(Time probeInterval, Time dreTime, Time onewayRttlow) {
		m_probeInterval = probeInterval;
		m_dreTime = dreTime;
		m_onewayRttLow = onewayRttlow;
	}

	/*************  源路由  *************/
	uint32_t ProteusRouting::GetOutPortFromPath(const uint32_t& path, const uint32_t& hopCount) {
		return ((uint8_t*)&path)[hopCount];
	}

	std::vector<uint32_t> ProteusRouting::GetPahtSet(uint32_t dstToRId, uint32_t nPath) {
		std::vector<uint32_t> nonCongested_path, undetermined_path, congested_path;
		std::vector<uint32_t> selected_path;
		uint32_t selectedCount = 0;
		// static double maxlu=0;


		std::map<uint32_t, proteusPathInfo>::iterator it;
		for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
			if (m_onewayRttLow.GetNanoSeconds() > it->second.oneway_rtt) {
				nonCongested_path.push_back(it->first);
			} else if(it->second.link_utilization < 0.6) {
				undetermined_path.push_back(it->first);
			} else {
				congested_path.push_back(it->first);
			}
			// if(it->second.link_utilization > maxlu) {
			// 	maxlu = it->second.link_utilization;
			// 	std::cout <<"maxlu:" << maxlu<< std::endl;
			// }
		}
		NS_ASSERT_MSG(m_proteusPathInfoTable.find(dstToRId)!=m_proteusPathInfoTable.end(), "m_proteusPathInfoTable can not find");
		

		// 从非拥塞路径集中取路径
		while (!nonCongested_path.empty()) {
			uint64_t min_rtt = UINT32_MAX;
			uint32_t min_idx = 0;

			if(selectedCount >= nPath) break;

			for (uint32_t i = 0; i < nonCongested_path.size(); i++) {
				uint64_t cur_rtt = m_proteusPathInfoTable[dstToRId][nonCongested_path[i]].oneway_rtt;
				if (cur_rtt < min_rtt) {
					min_rtt = cur_rtt;
					min_idx = i;
				}
			}

			selected_path.push_back(nonCongested_path[min_idx]);
			nonCongested_path.erase(nonCongested_path.begin() + min_idx);
			selectedCount++;						
		}

		// 从未确定路径集中去路径
		while (!undetermined_path.empty()) {
			double max_util = 0;
			uint32_t max_idx = 0;

			if(selectedCount >= nPath) break;

			for (uint32_t i = 0; i < undetermined_path.size(); i++) {
				double cur_util = m_proteusPathInfoTable[dstToRId][undetermined_path[i]].link_utilization;
				if (cur_util > max_util) {
					max_util = cur_util;
					max_idx = i;
				}
			}

			selected_path.push_back(undetermined_path[max_idx]);
			undetermined_path.erase(undetermined_path.begin() + max_idx);
			selectedCount++;						
		}

		// 从拥塞路径集中去路径
		while (!congested_path.empty()) {
			uint64_t min_rtt = UINT32_MAX;
			uint32_t min_idx = 0;

			if(selectedCount >= nPath) break;

			for (uint32_t i = 0; i < congested_path.size(); i++) {
				uint64_t cur_rtt = m_proteusPathInfoTable[dstToRId][congested_path[i]].oneway_rtt;
				if (cur_rtt < min_rtt) {
					min_rtt = cur_rtt;
					min_idx = i;
				}
			}

			selected_path.push_back(congested_path[min_idx]);
			congested_path.erase(congested_path.begin() + min_idx);
			selectedCount++;						
		}
		
		return selected_path;
	}

	// 根据proteus重路由逻辑 选出可以走的路径	
	uint32_t ProteusRouting::GetFinalPath(uint32_t dstToRId, std::vector<uint32_t> pathSet, CustomHeader& ch) {
		/*
		while {
			if(端口被暂停) {
				比较ttd和tqd 决定是否重路由
			}
		}		
		*/ 
	
		// return pathSet[0];

		// std::srand(Simulator::Now().GetInteger());
		// return pathSet[rand() % pathSet.size()];

		uint32_t selectedPath = pathSet[0];

		for (auto pathIt = pathSet.begin(); pathIt != pathSet.end(); pathIt++) {
			if (pathSet.size() == 1) {  // 只剩一条路径了 那就只能用它了
				selectedPath = *pathIt;
				break;
			}

			uint32_t interface = *pathIt & 0xFF;  // pathSet[0]为第一跳 即当前交换机输出端口

			bool* pauseStatus = GetInterfacePause(interface);
			if (!pauseStatus[ch.udp.pg]) {  // 可能没必要这么写吧 毕竟udp的优先级固定为3  非udp数据包也不会被proteus等负载均衡方法处理
				selectedPath = *pathIt;
				break;
			} else {  // 端口被暂停 执行proteus的重路由逻辑
				uint64_t ttd, tqd;

				ttd = m_proteusPathInfoTable[dstToRId][*pathIt].oneway_rtt - m_proteusPathInfoTable[dstToRId][*(pathIt+1)].oneway_rtt;
				tqd = GetInterfaceLoad(interface) * 8 * 1e9 / m_outPort2BitRateMap[interface];  // 乘1e9 转为纳秒单位
				// std::cout<< "ttd:" << ttd<<", tqd:" << tqd<<std::endl;
				std::cout << "Paused!  ";  // 看下这个逻辑触发的次数

				if (ttd > tqd) {
					selectedPath = *pathIt;
					std::cout << "select!	" << std::endl;
					// 标记需要更新ttd  真正转发时更新。不对 leaf-spine下 后续没有重路由的可能性 没必要更新ttd 没用
					break;
				} else {
					// 设置当前路径inactive 直到resume

					pathSet.erase(pathIt);
				}
			}
		}

		return selectedPath;
	}

	/*************  周期性主动探测  *************/ 
	void ProteusRouting::ProbeEvent() {
		// std::map<uint32_t, std::map<uint32_t, proteusPathInfo>>::iterator dstIter;
		// std::set<uint32_t>::iterator pathIter;
		// for (dstIter = m_proteusPathTable.begin(); dstIter != m_proteusPathTable.end(); dstIter++) {
		// 	for (pathIter = m_proteusRoutingTable.find(dstIter->first)->second.begin();  pathIter != m_proteusRoutingTable.find(dstIter->first)->second.end(); pathIter++) {
		// 		if (dstIter->second.find(*pathIter) == dstIter->second.end()) {  //没找到对应路径 插入
		// 			dstIter->second[*pathIter]
		// 		}
		// 	}
		// }
		std::map<uint32_t, std::set<uint32_t>>::iterator routeIter;
		std::set<uint32_t>::iterator pathIter;
		for (routeIter = m_proteusRoutingTable.begin(); routeIter != m_proteusRoutingTable.end(); routeIter++) {
			for(pathIter = routeIter->second.begin(); pathIter != routeIter->second.end(); pathIter++) {
				SendProbe(*pathIter, routeIter->first);
			}
		}


		m_probeEvent = Simulator::Schedule(m_probeInterval, &ProteusRouting::ProbeEvent, this);
	}

	void ProteusRouting::SendProbe(uint32_t pathId, uint32_t dstToRId) {
		// 探测包 按源路由路径 正常udp的优先级
		Ptr<Packet> packet = Create<Packet>(0);

		/*************  一些必要添加的信息  *************/
		CustomHeader dummy_ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		// dummy_ch.sip = Settings::node_id_to_ip(m_switch_id).Get();
		// dummy_ch.dip = Settings::node_id_to_ip(dstToRId).Get();
		packet->AddHeader(dummy_ch);


		// 以下ppp和ipv4头 是必须的？源路由应该用不上
		Ipv4Header ipv4h;
		ipv4h.SetSource(Settings::node_id_to_ip(m_switch_id));
		ipv4h.SetDestination(Settings::node_id_to_ip(dstToRId));
		ipv4h.SetProtocol(0x11);  // UDP
		ipv4h.SetTtl(64);
		ipv4h.SetPayloadSize(packet->GetSize());
		ipv4h.SetIdentification(UniformVariable(0, 65536).GetValue());
		packet->AddHeader(ipv4h);

		PppHeader ppp;
		ppp.SetProtocol(0x0021);
		packet->AddHeader(ppp);

		// ？
		packet->AddPacketTag(FlowIdTag(0));


		/*************  proteus自己的Tag  *************/
		ProteusTag proteusTag;
		proteusTag.SetHopCount(0);
		proteusTag.SetPathId(pathId);
		packet->AddPacketTag(proteusTag);

		ProteusProbeTag probingTag;
		probingTag.SetTimestampTx(Simulator::Now().GetNanoSeconds());
		packet->AddPacketTag(probingTag);

		uint32_t outport = GetOutPortFromPath(pathId, 0);

		// 虽然这里是DoSwitchSend 但最后一跳是SwitchNode::SendToDevContinue
		DoSwitchSend(packet, dummy_ch, outport, 3);

		return;
	}

	void ProteusRouting::DreEvent() {
		std::map<uint32_t, uint32_t>::iterator itr = m_DreMap.begin();
		for (; itr != m_DreMap.end(); ++itr) {
			uint32_t newX = itr->second * (1 - m_alpha);
			itr->second = newX;
		}
		NS_LOG_FUNCTION(Simulator::Now());
		m_dreEvent = Simulator::Schedule(m_dreTime, &ProteusRouting::DreEvent, this);
	}

	uint32_t ProteusRouting::UpdateLocalDre(Ptr<Packet> p, CustomHeader ch, uint32_t port) {
		uint32_t X = m_DreMap[port];
		uint32_t newX = X + p->GetSize();
		// NS_LOG_FUNCTION("Old X" << X << "New X" << newX << "port" << port << "Switch" <<
		// m_switch_id << Simulator::Now());
		m_DreMap[port] = newX;
		return newX;
	}

	double ProteusRouting::GetInPortUtil(uint32_t inport) {
		uint32_t X = m_DreMap[inport];
		uint64_t bitRate = m_outPort2BitRateMap[inport];
		
		double ratio;
		ratio = static_cast<double>(X * 8) / (bitRate * m_dreTime.GetSeconds() / m_alpha);

		return ratio;
	}

	void ProteusRouting::SetLinkCapacity(uint32_t outPort, uint64_t bitRate) {
		auto it = m_outPort2BitRateMap.find(outPort);
		if (it != m_outPort2BitRateMap.end()) {
			// already exists, then check matching
			NS_ASSERT_MSG(it->second == bitRate,
						  "bitrate already exists, but inconsistent with new input");
		} else {
			m_outPort2BitRateMap[outPort] = bitRate;
		}
	}

	void ProteusRouting::DoDispose() {
		m_probeEvent.Cancel();
	}







	/**************************  Proteus-Tag **************************/
	
    ProteusTag::ProteusTag() {}
    ProteusTag::~ProteusTag() {}

    TypeId ProteusTag::GetTypeId(void) {
		static TypeId tid = 
			TypeId("ns3::ProteusTag")
				.SetParent<Tag>()
				.AddConstructor<ProteusTag>();
	
		return tid;
	}

	/*************  继承Tag必须实现的函数  *************/
	TypeId ProteusTag::GetInstanceTypeId(void) const { return GetTypeId(); }
	uint32_t ProteusTag::GetSerializedSize(void) const {
		return sizeof(uint32_t) + sizeof(uint32_t);
	}
	void ProteusTag::Serialize(TagBuffer i) const {
		i.WriteU32(m_hopCount);
		i.WriteU32(m_pathId);
	}
	void ProteusTag::Deserialize(TagBuffer i) {
		m_hopCount = i.ReadU32();
		m_pathId = i.ReadU32();
	}
	void ProteusTag::Print(std::ostream &os) const {}

	/*************  源路由  *************/
	void ProteusTag::SetHopCount(uint32_t hopCount) { m_hopCount = hopCount;}
    uint32_t ProteusTag::GetHopCount(void) const { return m_hopCount; }
	void ProteusTag::SetPathId(uint32_t pathId) { m_pathId = pathId; }
	uint32_t ProteusTag::GetPathId(void) const { return m_pathId; }





	/**************************  ProteusProbe-Tag **************************/

	ProteusProbeTag::ProteusProbeTag() {}
    ProteusProbeTag::~ProteusProbeTag() {}

	TypeId ProteusProbeTag::GetTypeId(void) {
		static TypeId tid = 
			TypeId("ns3::ProteusProbeTag")
				.SetParent<Tag>()
				.AddConstructor<ProteusProbeTag>();
		
		return tid;
	}

	/*************  继承Tag必须实现的函数  *************/
	TypeId ProteusProbeTag::GetInstanceTypeId(void) const { return GetTypeId(); }
	uint32_t ProteusProbeTag::GetSerializedSize(void) const {
		return sizeof(uint64_t);
	}
	void ProteusProbeTag::Serialize(TagBuffer i) const {
		i.WriteU64(m_timestampTx);
	}
	void ProteusProbeTag::Deserialize(TagBuffer i) {
		m_timestampTx = i.ReadU64();
	}
	void ProteusProbeTag::Print(std::ostream& os) const {}

	
	/*************  路径信息  *************/
	void ProteusProbeTag::SetTimestampTx(uint64_t timestamp) { m_timestampTx = timestamp; }
    uint64_t ProteusProbeTag::GetTimestampTx(void) const { return m_timestampTx; }
	





	/**************************  ProteusFeedback-Tag **************************/

	ProteusFeedbackTag::ProteusFeedbackTag() {}
    ProteusFeedbackTag::~ProteusFeedbackTag() {}

	TypeId ProteusFeedbackTag::GetTypeId(void) {
		static TypeId tid = 
			TypeId("ns3::ProteusFeedbackTag")
				.SetParent<Tag>()
				.AddConstructor<ProteusFeedbackTag>();
		
		return tid;
	}

	/*************  继承Tag必须实现的函数  *************/
	TypeId ProteusFeedbackTag::GetInstanceTypeId(void) const { return GetTypeId(); }
	uint32_t ProteusFeedbackTag::GetSerializedSize(void) const {
		return sizeof(uint32_t) + sizeof(uint64_t) + sizeof(double);
	}
	void ProteusFeedbackTag::Serialize(TagBuffer i) const {
		i.WriteU32(m_fbPathId);
		i.WriteU64(m_oneway_rtt);
		i.WriteDouble(m_utilization);
	}
	void ProteusFeedbackTag::Deserialize(TagBuffer i) {
		m_fbPathId = i.ReadU32();
		m_oneway_rtt = i.ReadU64();
		m_utilization = i.ReadDouble();
	}
	void ProteusFeedbackTag::Print(std::ostream& os) const {}


	/*************  设置Tag信息  *************/
	void ProteusFeedbackTag::SetFbPathId(uint32_t pathId) {
		m_fbPathId = pathId;
	}
	uint32_t ProteusFeedbackTag::GetFbPahtId(void) const {
		return m_fbPathId;
	}

	
	void ProteusFeedbackTag::SetPathRtt(uint64_t oneway_rtt) {
		m_oneway_rtt = oneway_rtt;
	}
	uint64_t ProteusFeedbackTag::GetPathRtt(void) const {
		return m_oneway_rtt;
	}
	
	void ProteusFeedbackTag::SetPathUtilization(double utilization) {
		m_utilization = utilization;
	}
	double ProteusFeedbackTag::GetUtilization(void) const {
		return m_utilization;
	}


}
