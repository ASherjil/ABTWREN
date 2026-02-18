//
// Created by asherjil on 2/17/26.
//

#include "WRENReceiver.hpp"


WRENReceiver::WRENReceiver(const RingConfig& cfg /* TODO: SPSCQueue& queue*/)
  :m_ethernetSocket{cfg}{

}
WRENReceiver::WRENReceiver(WRENReceiver&& other) noexcept
  :m_ethernetSocket{std::move(other.m_ethernetSocket)}{

}

WRENReceiver& WRENReceiver::operator=(WRENReceiver&& other) noexcept{
  if (this != &other) {
    m_ethernetSocket = std::move(other.m_ethernetSocket);
  }
  return *this;
}