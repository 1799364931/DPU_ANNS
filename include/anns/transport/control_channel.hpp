// transport/control_channel.hpp：控制消息通道及软件模式的有界邮箱实现。
// 发送成功表示通道已接收消息，具体传输完成由 progress 推进。

#pragma once
#include "anns/protocol/messages.hpp"
#include "anns/transport/read_transport.hpp"
namespace anns {
class ControlChannel {
 public:
  virtual ~ControlChannel() = default;
  virtual bool send(const Bytes &bytes) = 0;
  virtual bool receive(Bytes &bytes) = 0;
  virtual void progress() = 0;
  virtual uint64_t capacity() const = 0;
};
struct Mailbox {
  std::deque<Bytes> inbox;
};
class SimulatedControlChannel final : public ControlChannel {
  Mailbox &incoming_;
  Mailbox &outgoing_;
  uint64_t capacity_;

 public:
  SimulatedControlChannel(Mailbox &in, Mailbox &out, uint64_t capacity)
      : incoming_(in), outgoing_(out), capacity_(capacity) {}
  // 复制消息到对端有界邮箱；队列满时返回背压。
  bool send(const Bytes &bytes) override {
    require(bytes.size() <= capacity_, "control message too large");
    if (outgoing_.inbox.size() >= 8) return false;
    outgoing_.inbox.push_back(bytes);
    return true;
  }
  // 取走一条消息并转移其缓冲所有权，空邮箱返回 false。
  bool receive(Bytes &bytes) override {
    if (incoming_.inbox.empty()) return false;
    bytes = std::move(incoming_.inbox.front());
    incoming_.inbox.pop_front();
    return true;
  }
  void progress() override {}
  uint64_t capacity() const override { return capacity_; }
};
void send_message(ControlChannel &channel, const Message &message);
}  // namespace anns
