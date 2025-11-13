#include "ns3/proteus-routing.h"

#include <iostream>
#include <random>

#include "ns3/log.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ppp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/random-variable.h"
#include "flow-stat-tag.h"


NS_LOG_COMPONENT_DEFINE("ProteusRouing");

namespace ns3 {

    ProteusRouting::ProteusRouting() {
		m_isToR = false;
		m_switch_id = (uint32_t)-1;
		// 发包间隔							   srcToR:sport-->dstToR:dport  timegap
		ftxgap = fopen("mix/debug/ftxgap.txt", "a+");
		// 路径rtt更新(从捎带获得)间隔			curToR-->dstToR:pathid gaptime
		fupdategap = fopen("mix/debug/fupdateGap.txt", "a+");
		// src->dst的每条路径全都更新 的间隔	curToR->dstToR:gaptime
		fallupdategap = fopen("mix/debug/fallupdateGap.txt", "a+");
		// getbetter中txdiff>rttdiff时的txdiff
		ftxdiff = fopen("mix/debug/ftxdiff.txt", "a+");
		// 每次的路径初选						src->dst:pathid pathrtt
		fpathselect = fopen("mix/debug/fpathselect.txt", "a+");
		// 每次选路的并发流统计
		fconflow = fopen("mix/debug/fconflow.txt", "a+");
	
		// set constants
		m_dreTime = Time(MicroSeconds(200));
		m_alpha = 0.2;
		m_onewayRttLow = NanoSeconds(4160*1.3);
		m_linkUtilThreshold = 0.7;
		m_nsample = 4;
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
		
		FlowIdTag inportTag;
		p->PeekPacketTag(inportTag);
		uint32_t inport = inportTag.GetFlowId();

		FlowStatTag fst;
		bool foundFlowStatTag = p->PeekPacketTag(fst);

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
				Settings::count_packet[0]++;
				// 还未捎带过 将捎带索引指向开头
				if (m_proteusFbPathPtr.find(dstToRId) == m_proteusFbPathPtr.end()) {
					m_proteusFbPathPtr[dstToRId] = 0;
				}
				
				// 捎带路径信息
				std::vector<std::tuple<uint32_t, bool, uint64_t>>::iterator it;
				if (m_proteusFbPathTable.find(dstToRId) != m_proteusFbPathTable.end()) {  // findout 就有至少一条路径信息
					it = m_proteusFbPathTable.find(dstToRId)->second.begin();				
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
						} else {
							it++;
							m_proteusFbPathPtr[dstToRId]++;
							if (it == m_proteusFbPathTable.find(dstToRId)->second.end()) {
								it = m_proteusFbPathTable.find(dstToRId)->second.begin();
								m_proteusFbPathPtr[dstToRId] = 0;
							}
						}											
					}
			
					if (found) {
						// 找到就捎带
						ProteusFeedbackTag proteusFbTag;
						proteusFbTag.SetFbPathId(std::get<0>(*it));
						proteusFbTag.SetPathRtt(std::get<2>(*it));

						uint32_t fbPathInport = m_pathId2Port[proteusFbTag.GetFbPahtId()];
						double utilization = GetInPortUtil(fbPathInport);
						proteusFbTag.SetPathUtilization(utilization);

						p->AddPacketTag(proteusFbTag);
					}
				}

				updatePathStatus(dstToRId);				

				std::set<uint32_t> pathSet = m_proteusRoutingTable[dstToRId];
				
				// debug：随机选一条path
				std::srand(static_cast<unsigned int>(ch.sip | (ch.dip << 16)));
				uint32_t selectedPath = *(std::next(pathSet.begin(), rand() % pathSet.size()));
				std::vector<uint32_t> selectedPathSet;

#if(1)  // per-flow
				
				uint64_t qpkey = ((uint64_t)ch.dip << 32) | ((uint64_t)ch.udp.sport << 16) | (uint64_t)ch.udp.pg | (uint64_t)ch.udp.dport;
				// qpkey = (uint64_t)EcmpHash(ch, 12, m_switch_id);  // 没区别
				if (m_proteusFlowTable.find(qpkey) != m_proteusFlowTable.end()) {
					selectedPath = m_proteusFlowTable[qpkey];
				} else {
					selectedPathSet = GetPathSet(dstToRId, m_nsample);
					selectedPath = GetFinalPath(dstToRId, selectedPathSet, ch);
					
					m_proteusFlowTable[qpkey] = selectedPath;
				}
				m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += proteus_perpacketdelay;
#else  // per-packet
				// per-packet
				uint64_t qpkey = ((uint64_t)ch.dip << 32) | ((uint64_t)ch.udp.sport << 16) | (uint64_t)ch.udp.pg | (uint64_t)ch.udp.dport;
				if (m_proteusFlowTable.find(qpkey) != m_proteusFlowTable.end()) {
					selectedPath = m_proteusFlowTable[qpkey];
					uint32_t betterPath = selectedPath;

					m_proteusFlowPackets[qpkey] += 1;

#if(0)  // 添加重路由限制条件
					// if (m_proteusRerouteRecord[qpkey] < 1) {  // 限制最大重路由次数
					// if ((now.GetNanoSeconds()-m_proteusFlowLastSelectTime[qpkey]) > 200000) {  // 限制重路由间隔
					if (m_proteusFlowPackets[qpkey] >= 15) {  // 传输一定数量的包后才能重路由
					// if (m_proteusRerouteRecord[qpkey] < 1 && m_proteusFlowPackets[qpkey] >= 50) {
					// if (now.GetNanoSeconds() - m_proteusFlowLastTxTime[qpkey] > 1000) {  // 发包间隔超过一定阈值才能重路由
					// if (now.GetNanoSeconds() - m_proteusFlowLastTxTime[qpkey] > m_deltaBetterRtt) {  // 发包间隔和getbetter的Δ相等
					// if (now.GetNanoSeconds() - m_proteusFlowLastTxTime[qpkey] > 200
					// 	&& now.GetNanoSeconds()-m_proteusFlowLastSelectTime[qpkey] > 200000) {
#else  // 不用额外的限制条件
					{
#endif
						// 若是有更好的路径
						if (getBetterPath(now.GetNanoSeconds(), m_proteusFlowLastTxTime[qpkey], dstToRId, betterPath)) {  // betterPath接收返回值
						// if (getBetterPath_Temp(now.GetNanoSeconds(), m_proteusFlowLastTxTime[qpkey], dstToRId, betterPath)) {
							m_proteusRerouteRecord[qpkey] += 1;
							if (m_proteusRerouteRecord[qpkey] == 1) Settings::count_packet[2] += 1;
							
							selectedPath = betterPath;
							RecordPathType(dstToRId, selectedPath);
							m_proteusFlowLastSelectTime[qpkey] = now.GetNanoSeconds();
							m_proteusFlowPackets[qpkey] = 0;  // 置零重新统计
							Settings::count_packet[1]++;
						}
					}
				} else {  // 新flow
					selectedPathSet = GetPathSet(dstToRId, m_nsample);
					selectedPath = GetFinalPath(dstToRId, selectedPathSet, ch);						
					// auto rtt1297 = m_proteusPathInfoTable[132][1297].oneway_rtt;
					// auto rtt1298 = m_proteusPathInfoTable[132][1298].oneway_rtt;
					// auto rtt1299 = m_proteusPathInfoTable[132][1299].oneway_rtt;
					// auto rtt1300 = m_proteusPathInfoTable[132][1300].oneway_rtt;
					// auto rtt1301 = m_proteusPathInfoTable[132][1301].oneway_rtt;
					// auto rtt1302 = m_proteusPathInfoTable[132][1302].oneway_rtt;
					// auto rtt1303 = m_proteusPathInfoTable[132][1303].oneway_rtt;
					// auto rtt1304 = m_proteusPathInfoTable[132][1304].oneway_rtt;
					#if(proteus_switch_fprintf)
					fprintf(fpathselect, "%u->%u:%u  %llu\n", srcToRId, dstToRId, selectedPath, m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt);
					#endif
					RecordPathType(dstToRId, selectedPath);
					m_proteusFlowLastSelectTime[qpkey] = now.GetNanoSeconds();
					m_proteusRerouteRecord[qpkey] = 0;
					m_proteusFlowPackets[qpkey] = 1;
					m_proteusRttIncTable[dstToRId][selectedPath] = 0;
				}
				m_proteusFlowTable[qpkey] = selectedPath;				

#if(1)  // 随着发包动态降低路径“优先级”									
				switch (1)
				{
				case 1:  // 每包固定增加rtt
					{
						#if(proteus_PATHRTTINC)
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += proteus_perpacketdelay;
						#else
						m_proteusRttIncTable[dstToRId][selectedPath] += proteus_perpacketdelay;
						#endif
					}
					break;
				case 2:  // 根据包大小增加
					{
						double alpha = 2000;
						double Inc = alpha*p->GetSize()*8/400;
						// p->GetSize()典型值是1048
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += Inc;  // 乘一个乘法因子
					}
					break;
				case 3:  // 指数平滑
					{
						uint64_t lastRtt = m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt;
						double alpha = 0.9;
						uint64_t baseInc = 500;
						// m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt = (1-alpha)*lastRtt + alpha*(lastRtt+baseInc);
						// 等同于下式......这不就是每包...
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += alpha * baseInc;
					}
					break;
				case 4:  // 指数单调增（错误版 值增长过快）
					{					
						uint64_t baseInc = 10;
						double growRate = 0.0007;
						double expInc = baseInc * exp(growRate * m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt);
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += expInc;
					}
					break;
				case 5:  // 修正版：以us为单位计算
					{
						double baseInc = 20;
						double growRate = 2e-3;
						uint64_t curRtt_us = m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt / 1000;
						double expInc = baseInc * exp(growRate * curRtt_us);
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += expInc*1000;  // 转回ns单位
					}
					break;
				case 6:  // 基于当前rtt
					{
						uint64_t curRtt = m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt;
						double beita = 1.2;
						double baseInc = beita*p->GetSize()*8/100;
						// baseInc = 80;
						double alpha = 0.005;
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += baseInc * (1 + (curRtt*alpha));
					}
					break;
				case 7:
					{
						double baseInc = 1000;  // 基础增长系数
						double scale = 1e3; // 缩放因子（控制增长拐点）
						uint64_t curRtt = m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt;
						
						// 对数增长公式：增量 = baseInc * log(1 + curRtt/scale)
						double logInc = baseInc * log(1 + curRtt / scale);
						m_proteusPathInfoTable[dstToRId][selectedPath].oneway_rtt += static_cast<uint64_t>(logInc);
					}
					break;
				default:
					break;
				}
#endif
				if (m_proteusFlowLastTxTime[qpkey] != 0)
					if (now.GetNanoSeconds() > m_proteusFlowLastTxTime[qpkey]) {
						#if(proteus_switch_fprintf)
						// fprintf(ftxgap, "switch id: %u, src id: %u, dst id: %u, sport: %u, dport: %u, timegap: %lu\n", m_switch_id, srcToRId, dstToRId, ch.udp.sport, ch.udp.dport, now.GetNanoSeconds()-m_proteusFlowLastTxTime[qpkey]);
						fprintf(ftxgap, "%u:%u-->%u:%u  %lu\n", srcToRId, ch.udp.sport, dstToRId, ch.udp.dport, now.GetNanoSeconds()-m_proteusFlowLastTxTime[qpkey]);
						#endif
					}
					else {
						#if(proteus_switch_fprintf)
						fprintf(ftxgap, "now < lastTxTime!!!\n");
						#endif
					}
				m_proteusFlowLastTxTime[qpkey] = now.GetNanoSeconds();
#endif

				// 记录选路时 并发流情况 用以比较选路和并发流数目关系
				uint32_t firstpath = *(m_proteusRoutingTable[dstToRId].begin());
				fprintf(fconflow, "%u:%u(%u,%u,%u,%u,%u,%u,%u,%u)\n", selectedPath, m_proteusConFlowTable[dstToRId][selectedPath],
					m_proteusConFlowTable[dstToRId][firstpath],
					m_proteusConFlowTable[dstToRId][firstpath+1],
					m_proteusConFlowTable[dstToRId][firstpath+2],
					m_proteusConFlowTable[dstToRId][firstpath+3],
					m_proteusConFlowTable[dstToRId][firstpath+4],
					m_proteusConFlowTable[dstToRId][firstpath+5],
					m_proteusConFlowTable[dstToRId][firstpath+6],
					m_proteusConFlowTable[dstToRId][firstpath+7]);
				// 统计并发流（在选路后进行 以便分析并发流数和选路的关系）
				if (foundFlowStatTag) {
					uint8_t flowstat = fst.GetType();
					switch (flowstat)
					{
					case FlowStatTag::FLOW_START:
						m_proteusConFlowTable[dstToRId][selectedPath] += 1;
						break;
					case FlowStatTag::FLOW_END:
						if (m_proteusConFlowTable[dstToRId][selectedPath] > 0)
							m_proteusConFlowTable[dstToRId][selectedPath] -= 1;
						break;
					default:
						break;
					}					
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
				m_pathId2Port[proteusTag.GetPathId()] = inport;

				UpdateLocalDre(p, ch, inport);


				ProteusFeedbackTag proteusFbTag;
				bool foundProteusFbTag = p->PeekPacketTag(proteusFbTag);

				// 若是收到反馈的路径信息
				if (foundProteusFbTag) {
					uint32_t fbPathId = proteusFbTag.GetFbPahtId();
					double maxUtil = std::max(proteusFbTag.GetUtilization(), GetInPortUtil(fbPathId & 0xFF));
					// if (maxUtil == proteusFbTag.GetUtilization()) Settings::count_maxutil[0]++;
					// else Settings::count_maxutil[1]++;
					struct proteusPathInfo pathInfo = {proteusFbTag.GetPathRtt(), maxUtil};					
					// struct proteusPathInfo pathInfo = {proteusFbTag.GetPathRtt()+(rand()%20000), maxUtil};  // 加随机值 没啥效果..
					
					// 记录下更新间隔
					if (m_proteusPathInfoLU.find(srcToRId) != m_proteusPathInfoLU.end())
						if (m_proteusPathInfoLU[srcToRId].find(fbPathId) != m_proteusPathInfoLU[srcToRId].end()) {
							#if(proteus_switch_fprintf)
							fprintf(fupdategap, "%u-->%u:%u %llu\n", m_switch_id, srcToRId, fbPathId, now.GetNanoSeconds() - m_proteusPathInfoLU[srcToRId][fbPathId]);
							#endif
						}
					m_proteusPathInfoLU[srcToRId][fbPathId] = now.GetNanoSeconds();
					
					// 记录到dstToR的所有路径都更新的时间间隔
					m_proteusAllPathUpdateFlag[srcToRId] |= 1 << ((fbPathId%256) - 17);
					m_proteusPathInfoStore[srcToRId][fbPathId] = proteusFbTag.GetPathRtt();  // 暂存rtt 等全部都更新了再更新PahtInfoTable			
					// if (m_proteusAllPathUpdateFlag[srcToRId]==0xFF || 
						// now.GetNanoSeconds() - m_proteusAllPathLastUpdate[srcToRId] > 5000) {
					if (m_proteusAllPathUpdateFlag[srcToRId]==0xFF) {
						if (m_proteusAllPathLastUpdate.find(srcToRId) != m_proteusAllPathLastUpdate.end()) {
							#if(proteus_switch_fprintf)
							fprintf(fallupdategap, "%u-->%u:%lu\n", m_switch_id, srcToRId , now.GetNanoSeconds() - m_proteusAllPathLastUpdate[srcToRId]);
							#endif
							// std::cout << m_switch_id << "-->" << srcToRId << ":" << now.GetNanoSeconds() - m_proteusAllPathLastUpdate[srcToRId] << std::endl;
						}
						m_proteusAllPathLastUpdate[srcToRId] = now.GetNanoSeconds();
						m_proteusAllPathUpdateFlag[srcToRId] = 0;

						for (auto it = m_proteusPathInfoStore[srcToRId].begin(); it != m_proteusPathInfoStore[srcToRId].end(); it++) {
							// m_proteusPathInfoTable[srcToRId][it->first].oneway_rtt = it->second;
						} 
					}
					
					// 便于调试观察变化
					// auto rtt1297 = m_proteusPathInfoTable[132][1297].oneway_rtt;
					// auto rtt1298 = m_proteusPathInfoTable[132][1298].oneway_rtt;
					// auto rtt1299 = m_proteusPathInfoTable[132][1299].oneway_rtt;
					// auto rtt1300 = m_proteusPathInfoTable[132][1300].oneway_rtt;
					// auto rtt1301 = m_proteusPathInfoTable[132][1301].oneway_rtt;
					// auto rtt1302 = m_proteusPathInfoTable[132][1302].oneway_rtt;
					// auto rtt1303 = m_proteusPathInfoTable[132][1303].oneway_rtt;
					// auto rtt1304 = m_proteusPathInfoTable[132][1304].oneway_rtt;

					// 更新路径信息
 					m_proteusPathInfoTable[srcToRId][fbPathId] = pathInfo;
					m_proteusOriPathInfoTable[srcToRId][fbPathId] = pathInfo;
					// m_proteusRttIncTable[dstToRId][fbPathId] = 0;
					m_proteusRttIncTable[srcToRId][fbPathId] = 0;
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
		return m_getInterfaceLoadCallback(interface);
	}

	/*************  获取port暂停状态 回调函数  *************/
	// 捆绑 SwitchNode::GetInterfacePauseStatus()
	void ProteusRouting::SetGetInterfacePauseCallback(GetInterfacePauseCallback callbackFunction) {
		m_getInterfacePauseCallback = callbackFunction;
	}

    bool* ProteusRouting::GetInterfacePause(uint32_t interface) {
		return m_getInterfacePauseCallback(interface);
	}

	/*************  交换机参数设置  *************/
	void ProteusRouting::SetSwitchInfo(bool isToR, uint32_t switch_id) {
		m_isToR = isToR;
    	m_switch_id = switch_id;
	}

	void ProteusRouting::SetConstants(Time probeInterval, uint64_t deltaBetterRtt, Time dreTime, Time onewayRttlow, double linkThreshold, uint32_t nsample) {
		m_probeInterval = probeInterval;
		m_deltaBetterRtt = deltaBetterRtt;
		m_dreTime = dreTime;
		m_onewayRttLow = onewayRttlow;
		m_linkUtilThreshold = linkThreshold;
		m_nsample = nsample;
	}

	/*************  源路由  *************/
	uint32_t ProteusRouting::GetOutPortFromPath(const uint32_t& path, const uint32_t& hopCount) {
		return ((uint8_t*)&path)[hopCount];
	}

	std::vector<uint32_t> ProteusRouting::GetPathSet(uint32_t dstToRId, uint32_t nPath) {
		std::vector<uint32_t> nonCongested_path, undetermined_path, congested_path;
		std::vector<uint32_t> selected_path;
		uint32_t selectedCount = 0;
		// static double maxlu=0;


		std::map<uint32_t, proteusPathInfo>::iterator it;

		// 测试直接选最低链路利用率路径
		// double min_lu = 1;
		// uint32_t temp_path;
		// for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
		// 	double cur_lu = it->second.link_utilization;
		// 	if (cur_lu < min_lu) {
		// 		min_lu = cur_lu;
		// 		temp_path = it->first;
		// 	}
		// }
		// selected_path.push_back(temp_path);
		// return selected_path;

		for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
			// false 表示路径为 inative
			if (m_proteusPathStatus[dstToRId][it->first] == false) {
				continue;
			}

			// 把路径全放入nonCongested集里 即只按rtt来选路 而非proteus的逻辑
			// nonCongested_path.push_back(it->first);

#if(1)
			if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[0]++;
			if (it->second.oneway_rtt < m_onewayRttLow.GetNanoSeconds()) {
				if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[1]++;
				nonCongested_path.push_back(it->first);
			} else if(it->second.link_utilization < m_linkUtilThreshold) {
				if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[2]++;
				undetermined_path.push_back(it->first);
			} else {
				if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[3]++;
				congested_path.push_back(it->first);
			}
#else
			if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[0]++;
			if (it->second.oneway_rtt < m_onewayRttLow.GetNanoSeconds() && !GetInterfacePause(it->first&0xFF)[3]) {
				if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[1]++;
				nonCongested_path.push_back(it->first);
			} else if(it->second.link_utilization < m_linkUtilThreshold || GetInterfacePause(it->first&0xFF)[3]) {
				if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[2]++;
				undetermined_path.push_back(it->first);
			} else {
				if (GetInterfacePause(it->first&0xFF)[3]) Settings::count_pfc[3]++;
				congested_path.push_back(it->first);
			}
#endif

			// if(it->second.link_utilization > maxlu) {
			// 	maxlu = it->second.link_utilization;
			// 	std::cout <<"maxlu:" << maxlu<< std::endl;
			// }
		}
		NS_ASSERT_MSG(m_proteusPathInfoTable.find(dstToRId)!=m_proteusPathInfoTable.end(), "m_proteusPathInfoTable can not find");

		// 按优先顺序从三个路径集随机选
		// while (!nonCongested_path.empty()) {
		// 	uint32_t idx = rand() % nonCongested_path.size();
		// 	selected_path.push_back(nonCongested_path[idx]);
		// 	nonCongested_path.erase(nonCongested_path.begin() + idx);
		// }
		// while (!undetermined_path.empty()) {
		// 	uint32_t idx = rand() % undetermined_path.size();
		// 	selected_path.push_back(undetermined_path[idx]);
		// 	undetermined_path.erase(undetermined_path.begin() + idx);
		// }
		// while (!congested_path.empty()) {
		// 	uint32_t idx = rand() % congested_path.size();
		// 	selected_path.push_back(congested_path[idx]);
		// 	congested_path.erase(congested_path.begin() + idx);
		// }
		// return selected_path;



		// 从非拥塞路径集中取路径
		while (!nonCongested_path.empty()) {
			uint64_t min_rtt = UINT32_MAX;
			std::vector<uint32_t> idx;
			idx.push_back(0);

			if(selectedCount >= nPath) break;

			for (uint32_t i = 0; i < nonCongested_path.size(); i++) {
				uint64_t cur_rtt = m_proteusPathInfoTable[dstToRId][nonCongested_path[i]].oneway_rtt;
				// uint64_t cur_rtt = m_proteusPathInfoTable[dstToRId][nonCongested_path[i]].oneway_rtt + m_proteusRttIncTable[dstToRId][nonCongested_path[i]];
				if (cur_rtt < min_rtt) {
					min_rtt = cur_rtt;

					idx.clear();
					idx.push_back(i);
				} else if (cur_rtt == min_rtt) {
					idx.push_back(i);
				}

				// uint32_t delta = 250;
				// if (cur_rtt+delta > min_rtt && min_rtt+delta > cur_rtt) {  // (min_rtt-500, min_rtt+500) min_rtt 一定范围内认为相等 比如500ns
				// 	idx.push_back(i);
				// } else if (cur_rtt+delta < min_rtt) {
				// 	min_rtt = cur_rtt;

				// 	idx.clear();
				// 	idx.push_back(i);
				// }
			}

			uint32_t rand_idx = rand() % idx.size();  // 从rtt相同的路径中随机
			selected_path.push_back(nonCongested_path[idx[rand_idx]]);
			nonCongested_path.erase(nonCongested_path.begin() + idx[rand_idx]);

			selectedCount++;
			idx.clear();
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
	uint32_t ProteusRouting::GetFinalPath(uint32_t dstToRId, std::vector<uint32_t> &pathSet, CustomHeader &ch) {
		// return pathSet[0];

		// std::srand(Simulator::Now().GetInteger());
		// return pathSet[rand() % pathSet.size()];

		uint32_t selectedPath = pathSet[0];

		// 若只有一条路径 直接返回
		if (pathSet.size() == 1) return pathSet[0];

		uint32_t i = 0;
		for (i = 0; i < pathSet.size()-1; i++) {
			uint32_t curPath = pathSet[i];
			uint32_t curIf = curPath & 0xFF;  // pathSet[0]为第一跳 即当前交换机输出端口

			bool* pauseStatus = GetInterfacePause(curIf);
			// 没被PFC暂停 不用继续比较ttd和tqd
			// if (!pauseStatus[ch.udp.pg]) {  // 可能没必要这么写吧(ch.udp.pg) 毕竟udp的优先级固定为3  非udp数据包也不会被proteus等负载均衡方法处理
			// 	selectedPath = curPath;
			// 	Settings::count_select[i]++;
			// 	break;
			// } else {  // 端口被暂停 执行proteus的重路由逻辑
				uint64_t ttd, tqd;

#if(1)
				if (m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt > m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt) {
					ttd = m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt - m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt;					
				// if (m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt+ m_proteusRttIncTable[dstToRId][pathSet[i+1]] > m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt+m_proteusRttIncTable[dstToRId][curPath]) {
					// ttd = m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt + m_proteusRttIncTable[dstToRId][pathSet[i+1]] - m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt-m_proteusRttIncTable[dstToRId][curPath];
					tqd = GetInterfaceLoad(curIf) * 8 * 1e9 / m_outPort2BitRateMap[curIf];  // 乘1e9 转为纳秒单位

					if (ttd >= tqd) { // 下一条路径带来的延迟 大于 在当前路径等待队列的延迟 因此 选择当前路径
					// if (ttd >= tqd || // 以下或条件效果不佳
					// m_proteusPathInfoTable[dstToRId][curPath].link_utilization < m_proteusPathInfoTable[dstToRId][pathSet[i+1]].link_utilization) {
						selectedPath = curPath;
						Settings::count_select[i]++;
						// 标记需要更新ttd  真正转发时更新。不对 leaf-spine下 后续没有重路由的可能性 没必要更新ttd 没用
						return selectedPath;
					} else {
						// 设置当前路径inactive 直到resume
						m_proteusPathStatus[dstToRId][curPath] = false;
					}
				} else {  // 若是次优路径的rtt更小 则在PFC暂停的情况下直接切换
					// if (pauseStatus[ch.udp.pg])  // 暂停时才设为inactive。 性能较差
						m_proteusPathStatus[dstToRId][curPath] = false;
				}
#else
				// 非拥塞路径为空 可能发生次优路径rtt小于最优路径rtt
				// if (m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt < m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt)
				// 	ttd = m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt - m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt;
				// else
					ttd = m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt - m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt;
				
				tqd = GetInterfaceLoad(curIf) * 8 * 1e9 / m_outPort2BitRateMap[curIf];  // 乘1e9 转为纳秒单位
				std::cout << "Paused!" << ttd<<","<<tqd<<"   ";  // 看下这个逻辑触发的次数
				// if (ttd==0) {
				// 	std::cout << "ttd==0: "<< m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt<<", "<<m_proteusPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt<<std::endl;
				// }

				if (ttd >= tqd) {
					selectedPath = curPath;
					Settings::count_select[i]++;
					std::cout << "select!	" << std::endl;
					// 标记需要更新ttd  真正转发时更新。不对 leaf-spine下 后续没有重路由的可能性 没必要更新ttd 没用
					break;
				} else {
					// 设置当前路径inactive 直到resume
					m_proteusPathStatus[dstToRId][curPath] = false;
				}
#endif
			// }
		}

		if (i == pathSet.size()-1) {  // 剩最后一条 返回最后一条路径
			Settings::count_select[i]++;
			return pathSet[i];
			
			// std::srand(Simulator::Now().GetInteger());
			// return pathSet[rand() % pathSet.size()];
		} else {
			return selectedPath;
		}
	}

	uint32_t ProteusRouting::GetFinalPath_Temp(uint32_t dstToRId, std::vector<uint32_t> &pathSet, CustomHeader &ch) {
		uint32_t selectedPath = pathSet[0];

		// 若只有一条路径 直接返回
		if (pathSet.size() == 1) return pathSet[0];

		uint32_t i = 0;
		for (i = 0; i < pathSet.size()-1; i++) {
			uint32_t curPath = pathSet[i];
			uint32_t curIf = curPath & 0xFF;  // pathSet[0]为第一跳 即当前交换机输出端口

			bool* pauseStatus = GetInterfacePause(curIf);

			uint64_t ttd, tqd;
			if (m_proteusOriPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt > m_proteusOriPathInfoTable[dstToRId][curPath].oneway_rtt) {
				ttd = m_proteusOriPathInfoTable[dstToRId][pathSet[i+1]].oneway_rtt - m_proteusOriPathInfoTable[dstToRId][curPath].oneway_rtt;					
				tqd = GetInterfaceLoad(curIf) * 8 * 1e9 / m_outPort2BitRateMap[curIf];  // 乘1e9 转为纳秒单位

				if (ttd >= tqd) { // 下一条路径带来的延迟 大于 在当前路径等待队列的延迟 因此 选择当前路径
					selectedPath = curPath;
					Settings::count_select[i]++;
					// 标记需要更新ttd  真正转发时更新。不对 leaf-spine下 后续没有重路由的可能性 没必要更新ttd 没用
					return selectedPath;
				} else {
					// 设置当前路径inactive 直到resume
					m_proteusPathStatus[dstToRId][curPath] = false;
				}
			} else {  // 若是次优路径的rtt更小 则在PFC暂停的情况下直接切换
				// if (pauseStatus[ch.udp.pg])  // 暂停时才设为inactive。 性能较差
					m_proteusPathStatus[dstToRId][curPath] = false;
			}
		}

		if (i == pathSet.size()-1) {  // 剩最后一条 返回最后一条路径
			Settings::count_select[i]++;
			return pathSet[i];
			
			// std::srand(Simulator::Now().GetInteger());
			// return pathSet[rand() % pathSet.size()];
		} else {
			return selectedPath;
		}
	}

	void ProteusRouting::RecordPathType(uint32_t dstToRId, uint32_t path) {
		std::map<uint32_t, proteusPathInfo>::iterator it;

		for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
			if (it->first == path) {
				Settings::count_select_path[0] += 1;
				if (it->second.oneway_rtt < m_onewayRttLow.GetNanoSeconds()) {
					Settings::count_select_path[1] += 1;
				} else if(it->second.link_utilization < m_linkUtilThreshold) {
					Settings::count_select_path[2] += 1;
				} else {
					Settings::count_select_path[3] += 1;
				}
			}
		}
	}

	bool ProteusRouting::getBetterPath(int64_t now, uint64_t lastTxTime, uint32_t dstToRId, uint32_t& betterPath) {
		uint32_t curPath = betterPath;
		uint64_t curPathRtt = m_proteusPathInfoTable[dstToRId][curPath].oneway_rtt;
		double curPathUtil = m_proteusPathInfoTable[dstToRId][curPath].link_utilization;
		std::vector<uint32_t> candidate_path;
		
		std::map<uint32_t, proteusPathInfo>::iterator it;
		bool res = 0;
		
#if(proteus_PATHRTTINC)
		for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
			bool pathStatus = m_proteusPathStatus[dstToRId][it->first];
			
			// if (pathStatus==true && it->second.oneway_rtt < ((curPathRtt>6000)?curPathRtt-6000:1)) {
			if (pathStatus==true && it->second.oneway_rtt+m_deltaBetterRtt < curPathRtt) {  // 在左边加更方便....
				uint64_t rttDiff = curPathRtt - it->second.oneway_rtt;
				uint64_t txDiff = now - lastTxTime;  // nanoseconds
				// if(txDiff>99999999) txDiff = 0;
				Settings::total_diff[0] += rttDiff;
				
				// 发包间隔大于路径延迟差(首先要大于m_deltaBetterRtt..)才能换路径 以避免失序
				// if (1) {
				if (txDiff > rttDiff) {
				// if (txDiff > rttDiff || txDiff>15000) {
					#if(proteus_switch_fprintf)
					fprintf(ftxdiff, "%lu\n", txDiff);
					#endif				
					res = 1;

					candidate_path.push_back(it->first);
					// candidate按rtt从小到大排序  // 不排序好像反而好(差距很小)
					// std::sort(candidate_path.begin(), candidate_path.end(), 
					// 			[this, dstToRId](uint32_t a, uint32_t b) {
					// 				return m_proteusPathInfoTable[dstToRId][a].oneway_rtt < m_proteusPathInfoTable[dstToRId][b].oneway_rtt;
					// 			});
				} else {					
					Settings::total_diff[1] += rttDiff;
				}
			} else if (pathStatus==true && it->second.oneway_rtt >= curPathRtt + 20000) {  // 大于等于curRtt也可 因为能避免失序
				// if (it->second.link_utilization < curPathUtil) {
				// 	candidate_path.push_back(it->first);
				// }
				
				// std::sort(candidate_path.begin(), candidate_path.end(), 
				// 			[this, dstToRId](uint32_t a, uint32_t b) {
				// 				return m_proteusPathInfoTable[dstToRId][a].oneway_rtt < m_proteusPathInfoTable[dstToRId][b].oneway_rtt;
				// 			});
			}
		}
#else
		curPathRtt += m_proteusRttIncTable[dstToRId][curPath];
		for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
			bool pathStatus = m_proteusPathStatus[dstToRId][it->first];
			uint64_t candidate_rtt = it->second.oneway_rtt + m_proteusRttIncTable[dstToRId][it->first];

			if (pathStatus==true && candidate_rtt+m_deltaBetterRtt < curPathRtt) {
				uint64_t rttDiff = curPathRtt - candidate_rtt;				
				uint64_t txDiff = now - lastTxTime;  // nanoseconds
				Settings::total_diff[0] += rttDiff;
				
				if (txDiff > rttDiff || txDiff>6000) {
					#if(proteus_switch_fprintf)
					fprintf(ftxdiff, "%lu\n", txDiff);
					#endif
					res = 1;

					candidate_path.push_back(it->first);
				}
				else
					Settings::total_diff[1] += rttDiff;
			}
		}

		// 每包rtt增加只用于路径初选。 复用宏proteus_PATHRTTINC为0的部分代码  将上面代码用这里的代码替换 还需改getfinal getpathset代码
		// for (it = m_proteusPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusPathInfoTable.find(dstToRId)->second.end(); it++) {
		// 	bool pathStatus = m_proteusPathStatus[dstToRId][it->first];
			
		// 	if (pathStatus==true && it->second.oneway_rtt+m_deltaBetterRtt < curPathRtt) {
		// 		uint64_t rttDiff = curPathRtt - it->second.oneway_rtt;
		// 		uint64_t txDiff = now - lastTxTime;  // nanoseconds
		// 		Settings::total_diff[0] += rttDiff;
				
		// 		// 发包间隔大于路径延迟差(首先要大于m_deltaBetterRtt..)才能换路径 以避免失序
		// 		if (txDiff > rttDiff) {
		// 			res = 1;

		// 			candidate_path.push_back(it->first);
		// 		} else {					
		// 			Settings::total_diff[1] += rttDiff;
		// 		}
		// 	}
		// }
#endif

		if (res == 1) {
			CustomHeader dummy_ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
			dummy_ch.udp.pg = 3;
			betterPath = GetFinalPath(dstToRId, candidate_path, dummy_ch);			
		}

		return res;
	}

	bool ProteusRouting::getBetterPath_Temp(int64_t now, uint64_t lastTxTime, uint32_t dstToRId, uint32_t& betterPath) {
		uint32_t curPath = betterPath;
		uint64_t curPathRtt = m_proteusOriPathInfoTable[dstToRId][curPath].oneway_rtt;
		double curPathUtil = m_proteusOriPathInfoTable[dstToRId][curPath].link_utilization;
		std::vector<uint32_t> candidate_path;
		
		std::map<uint32_t, proteusPathInfo>::iterator it;
		bool res = 0;
		
		for (it = m_proteusOriPathInfoTable.find(dstToRId)->second.begin(); it != m_proteusOriPathInfoTable.find(dstToRId)->second.end(); it++) {
			bool pathStatus = m_proteusPathStatus[dstToRId][it->first];
			
			if (pathStatus==true && it->second.oneway_rtt+m_deltaBetterRtt < curPathRtt) {
				uint64_t rttDiff = curPathRtt - it->second.oneway_rtt;
				uint64_t txDiff = now - lastTxTime;
				Settings::total_diff[0] += rttDiff;
				
				if (txDiff > rttDiff) {
				// if (txDiff > rttDiff || txDiff>6000) {
					#if(proteus_switch_fprintf)
					fprintf(ftxdiff, "%lu\n", txDiff);
					#endif				
					res = 1;

					candidate_path.push_back(it->first);
				} else {					
					Settings::total_diff[1] += rttDiff;
				}
			}
		}

		if (res == 1) {
			CustomHeader dummy_ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
			dummy_ch.udp.pg = 3;
			betterPath = GetFinalPath_Temp(dstToRId, candidate_path, dummy_ch);			
		}

		return res;
	}

	void ProteusRouting::updatePathStatus(uint32_t dstToRId) {
		std::map<uint32_t, bool>::iterator it = m_proteusPathStatus.find(dstToRId)->second.begin();
		for (; it != m_proteusPathStatus.find(dstToRId)->second.end(); it++) {
			if (it->second == false) {
				// 取的是it->first所指路径的第一跳接口对应的QbbNetDevice的第3条队列(udp的队列)是否被PFC暂停
				if (GetInterfacePause(it->first & 0xFF)[3] == false) {  // 若PFC恢复
					it->second = true;  // true表示active
				}
			}
		}
	}

	uint32_t ProteusRouting::EcmpHash(CustomHeader &ch, size_t len, uint32_t seed) {
		uint32_t h = seed;

		union {
			uint8_t u8[4 + 4 + 2 + 2];
			uint32_t u32[3];
		} buf;
		
		buf.u32[0] = ch.sip;
		buf.u32[1] = ch.dip;
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
		
		const uint8_t *key = buf.u8;

		if (len > 3) {
			const uint32_t *key_x4 = (const uint32_t *)key;
			size_t i = len >> 2;
			do {
				uint32_t k = *key_x4++;
				k *= 0xcc9e2d51;
				k = (k << 15) | (k >> 17);
				k *= 0x1b873593;
				h ^= k;
				h = (h << 13) | (h >> 19);
				h += (h << 2) + 0xe6546b64;
			} while (--i);
			key = (const uint8_t *)key_x4;
		}
		if (len & 3) {
			size_t i = len & 3;
			uint32_t k = 0;
			key = &key[i - 1];
			do {
				k <<= 8;
				k |= *key--;
			} while (--i);
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			h ^= k;
		}
		h ^= len;
		h ^= h >> 16;
		h *= 0x85ebca6b;
		h ^= h >> 13;
		h *= 0xc2b2ae35;
		h ^= h >> 16;
		return h;
	}
	

	/*************  周期性主动探测  *************/ 
	void ProteusRouting::ProbeEvent() {
		std::map<uint32_t, std::set<uint32_t>>::iterator routeIter;
		std::set<uint32_t>::iterator pathIter;
		for (routeIter = m_proteusRoutingTable.begin(); routeIter != m_proteusRoutingTable.end(); routeIter++) {
			for(pathIter = routeIter->second.begin(); pathIter != routeIter->second.end(); pathIter++) {
				SendProbe(*pathIter, routeIter->first);
			}
		}


		m_probeEvent = Simulator::Schedule(m_probeInterval, &ProteusRouting::ProbeEvent, this);
	}

	// todo: 完善创建的packet？
	void ProteusRouting::SendProbe(uint32_t pathId, uint32_t dstToRId) {
		// 探测包 按源路由路径 正常udp的优先级
		Ptr<Packet> packet = Create<Packet>(0);

		/*************  一些必要添加的信息  *************/
		CustomHeader dummy_ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		dummy_ch.sip = Settings::node_id_to_ip(m_switch_id).Get();
		dummy_ch.dip = Settings::node_id_to_ip(dstToRId).Get();
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
		m_dreEvent.Cancel();
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
