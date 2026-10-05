# pragma once
# include<lab/sim/InputCmd.h>
# include <optional>

// 离线教学占位：Recv 始终为空、Send 仅打印；不模拟网络或实际延迟。
class NetStub{
public:
    std::optional<InputCmd> Recv();
    
    void Send(const InputCmd &cmd);
    
    void SetFakeLatencyMs(int ms){fakeLatencyMs_s =ms;} // 当前只存值，不影响收发
private:
    int fakeLatencyMs_s = 0;
};
