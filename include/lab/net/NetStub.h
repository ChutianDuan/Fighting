# pragma once
# include<lab/sim/InputCmd.h>
# include <optional>

class NetStub{
public:
    std::optional<InputCmd> Recv();
    
    void Send(const InputCmd &cmd);
    
    void SetFakeLatencyMs(int ms){fakeLatencyMs_s =ms;}
private:
    int fakeLatencyMs_s = 0;
};
