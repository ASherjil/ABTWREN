//
// Created by asherjil on 2/17/26.
//

#include "WRENReceiver.hpp"


WRENReceiver::WRENReceiver(const RingConfig& cfg /* TODO: SPSCQueue& queue*/)
  :m_ethernetSocket{cfg}{

}
