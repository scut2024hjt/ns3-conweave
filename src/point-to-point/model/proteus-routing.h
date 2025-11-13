#ifndef __PROTEUS_ROUTING_H__
#define __PROTEUS_ROUTING_H__

// #include <iostream>

#include "ns3/address.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/net-device.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/settings.h"
#include "ns3/simulator.h"
#include "ns3/tag.h"

namespace ns3 {

#define proteus_switch_fprintf 0

#define proteus_perpacketdelay 1000
#define proteus_PATHRTTINC 1  // 1表示直接在m_proteusPathInfoTable加rtt，0表示在另外的表m_proteusRttIncTable加并只影响getbetter


struct proteusPathInfo {
    uint64_t oneway_rtt;
    double link_utilization;

    proteusPathInfo(uint64_t rtt = 0, double util = 0) : oneway_rtt(rtt), link_utilization(util) {};
};

class ProteusRouting: public Object {
    friend class SwitchMmu;
    friend class SwitchNode;

    public:
        ProteusRouting();
        ~ProteusRouting();
        
        // core function
		void RouteInput(Ptr<Packet> p, CustomHeader &ch);

        static TypeId GetTypeId(void);

        // callback of SwitchSend
        typedef Callback<void, Ptr<Packet>, CustomHeader&, uint32_t, uint32_t> SwitchSendCallback;
        typedef Callback<void, Ptr<Packet>, CustomHeader&> SwitchSendToDevCallback;
        void SetSwitchSendCallback(SwitchSendCallback switchSendCallback);  // set callback
        void SetSwitchSendToDevCallback(SwitchSendToDevCallback switchSendToDevCallback);  // set callback
        void DoSwitchSend(Ptr<Packet> p, CustomHeader& ch, uint32_t outDev, uint32_t qIndex);   // TxToR and Agg/CoreSw
        void DoSwitchSendToDev(Ptr<Packet> p, CustomHeader& ch);  // only at RxToR

        typedef Callback<uint32_t, uint32_t> GetInterfaceLoadCallback;
        void SetGetInterfaceLoadCallback(GetInterfaceLoadCallback callbackFunction);
        uint32_t GetInterfaceLoad(uint32_t interface);

        typedef Callback<bool*, uint32_t> GetInterfacePauseCallback;
        void SetGetInterfacePauseCallback(GetInterfacePauseCallback callbakcFunction);
        bool* GetInterfacePause(uint32_t interface);
        

        // SET functions
        void SetSwitchInfo(bool isToR, uint32_t switch_id);
        void SetConstants(Time probeInterval, uint64_t deltaBetterRtt, Time dreTime, Time onewayRttlow, double linkThreshold, uint32_t nsample);

        uint32_t GetOutPortFromPath(const uint32_t& path, const uint32_t& hopCount);

        // topological info (should be initialized in the beginning)
        std::map<uint32_t, std::set<uint32_t> > m_proteusRoutingTable;  // routing table (ToRId -> pathId) (stable)

        // public 以便在脚本中初始化
        // 存储路径信息的表  ToRId --> <pathId, pathInfo>
        std::map<uint32_t, std::map<uint32_t, struct proteusPathInfo>> m_proteusPathInfoTable;
        std::map<uint32_t, std::map<uint32_t, struct proteusPathInfo>> m_proteusOriPathInfoTable;
        std::map<uint32_t, std::map<uint32_t, uint64_t>> m_proteusRttIncTable;
        std::map<uint32_t, std::map<uint32_t, uint64_t>> m_proteusPathInfoLU;  // LU: last update
        std::map<uint32_t, int64_t> m_proteusAllPathLastUpdate;
        std::map<uint32_t, uint8_t> m_proteusAllPathUpdateFlag;  // dstToRId, allpath update flag
        std::map<uint32_t, std::map<uint32_t, uint64_t>> m_proteusPathInfoStore;
        std::map<uint32_t, std::map<uint32_t, bool>>m_proteusPathStatus;

        // 统计并发流（和每包rtt增加方法进行比较 故表类似PathInfoTable 即 <ToRId, pathId>:并发流数）
        std::map<uint32_t, std::map<uint32_t, uint32_t>> m_proteusConFlowTable;

        std::vector<uint32_t> GetPathSet(uint32_t dstToRId, uint32_t nPath);
        uint32_t GetFinalPath(uint32_t dstToRId, std::vector<uint32_t> &pathSet, CustomHeader &ch);
        uint32_t GetFinalPath_Temp(uint32_t dstToRId, std::vector<uint32_t> &pathSet, CustomHeader &ch);
        bool getBetterPath(int64_t now, uint64_t lastTxTime, uint32_t dstToRId, uint32_t& betterPath);
        bool getBetterPath_Temp(int64_t now, uint64_t lastTxTime, uint32_t dstToRId, uint32_t& betterPath);
        
        
        // periodic events
        EventId m_probeEvent;
        void ProbeEvent();
        void SendProbe(uint32_t pathId, uint32_t dstId);
        
        EventId m_dreEvent;
        void DreEvent();
        uint32_t UpdateLocalDre(Ptr<Packet> p, CustomHeader ch, uint32_t outPort);
        double GetInPortUtil(uint32_t inport);

        std::map<uint32_t, uint64_t> m_outPort2BitRateMap;  // outPort -> link bitrate (bps) (stable)
        void SetLinkCapacity(uint32_t outPort, uint64_t bitRate);

        // 记录传进来的包的pathId --> inPort
        std::map<uint32_t, uint32_t> m_pathId2Port;  // 其实至少leaf-spine可以不用这个：不同leaf连到同一spine的接口索引(设备号)是相同的。
                                                        // 所以其实get pathid的第一跳端口也是一样的


        virtual void DoDispose();
        

    private:
        bool m_isToR;
        uint32_t m_switch_id;
        FILE *ftxgap, *fupdategap, *ftxdiff, *fallupdategap, *fpathselect, *fpathrtt, *fconflow;

        // callback
        SwitchSendCallback m_switchSendCallback;  // bound to SwitchNode::SwitchSend
        SwitchSendToDevCallback m_switchSendToDevCallback;  // bound to SwitchNode::SendToDevContinue

        GetInterfaceLoadCallback m_getInterfaceLoadCallback;
        GetInterfacePauseCallback m_getInterfacePauseCallback;

        // proteus constants
        Time m_probeInterval;
        uint64_t m_deltaBetterRtt;

        // 发出探测包的ToRId --> <路径Id, 路径信息>
        // probeToRId --> <probePathId, updateFlag>  因为要轮询的方式进行捎带 所以用vector存
        // std::map<uint32_t, std::map<uint32_t, bool>> m_proteusFbPathFlag;
        // 即用来存储要piggyback的信息 也用来选路径 不对不行.. 同一条路径src2dst和dst2src是不同的pathId
        // probeToRId --> <probePathId --> PathInfo>
        // std::map<uint32_t, std::map<uint32_t, struct proteusPathInfo>> m_proteusFbPathInfo;
        std::map<uint32_t, std::vector<std::tuple<uint32_t, bool, uint64_t>>> m_proteusFbPathTable;  // 只需记录rtt 链路利用率捎带时获取
        // 记录上一次捎带的path的下一条path在vector的索引 即下一次要RR捎带开始的位置
        std::map<uint32_t, uint32_t> m_proteusFbPathPtr;  // ToRId --> vector_ptr 

        // 用于轮询选取当前piggyback哪个路径信息
        // probeToRId --> ptr_probePathId
        // std::map<uint32_t, std::map<uint32_t, std::pair<bool, struct proteusPathInfo>>::iterator> m_proteusFbPathItr;

        std::map<uint64_t, uint32_t>m_proteusFlowTable;  // QpKey --> pathId
        std::map<uint64_t, uint64_t>m_proteusFlowLastTxTime;  // qpkey(flow) --> lastTxTimestamp
        std::map<uint64_t, bool>m_proteusRerouteRecord;
        std::map<uint64_t, uint64_t>m_proteusFlowLastSelectTime;
        std::map<uint64_t, uint64_t>m_proteusFlowPackets;

        std::map<uint32_t, uint32_t> m_DreMap;  // inPort(spine->ToR) -> DRE (at DstToR)
        
        // constants
        double m_alpha;  // dre algorithm (e.g., 0.2)
        Time m_dreTime;  // dre alogrithm (e.g., 200us)
        Time m_onewayRttLow;
        double m_linkUtilThreshold;
        uint32_t m_nsample;

        void updatePathStatus(uint32_t dstToRId);
        void RecordPathType(uint32_t dstToRId, uint32_t path);

        uint32_t EcmpHash(CustomHeader &ch, size_t len, uint32_t seed);
};

class ProteusTag : public Tag {
    public:
        ProteusTag();
        ~ProteusTag();

        static TypeId GetTypeId(void);
        
        virtual TypeId GetInstanceTypeId(void) const;
        virtual uint32_t GetSerializedSize(void) const;
        virtual void Serialize(TagBuffer i) const;
        virtual void Deserialize(TagBuffer i);
        virtual void Print(std::ostream& os) const;

        void SetHopCount(uint32_t hopCount);
        uint32_t GetHopCount(void) const;
        void SetPathId(uint32_t pathId);
        uint32_t GetPathId(void) const;
    
    private:
        uint32_t m_hopCount;
        uint32_t m_pathId;
};

class ProteusProbeTag : public Tag {
    public:
        ProteusProbeTag();
        ~ProteusProbeTag();

        static TypeId GetTypeId(void);
        
        virtual TypeId GetInstanceTypeId(void) const;
        virtual uint32_t GetSerializedSize(void) const;
        virtual void Serialize(TagBuffer i) const;
        virtual void Deserialize(TagBuffer i);
        virtual void Print(std::ostream& os) const;

        void SetTimestampTx(uint64_t timestamp);
        uint64_t GetTimestampTx(void) const;
    
    private:
        uint64_t m_timestampTx;
};

class ProteusFeedbackTag : public Tag {
    public:
        ProteusFeedbackTag();
        ~ProteusFeedbackTag();

        static TypeId GetTypeId(void);
        
        virtual TypeId GetInstanceTypeId(void) const;
        virtual uint32_t GetSerializedSize(void) const;
        virtual void Serialize(TagBuffer i) const;
        virtual void Deserialize(TagBuffer i);
        virtual void Print(std::ostream& os) const;

        void SetFbPathId(uint32_t pathId);
        void SetPathRtt(uint64_t oneway_rtt);
        void SetPathUtilization(double utilization);

        uint32_t GetFbPahtId(void) const;
        uint64_t GetPathRtt(void) const;
        double GetUtilization(void) const;

    
    private:
        uint32_t m_fbPathId;
        // struct proteusPathInfo m_fbPathInfo;
        uint64_t m_oneway_rtt;
        double m_utilization;
};

}

#endif