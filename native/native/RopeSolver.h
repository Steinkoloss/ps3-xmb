#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// One private guest address space. No PS3 OS, emulator, or captured animation.
class RopeSolver {
public:
    using Nodes = std::array<std::array<float, 4>, 361>;
    RopeSolver(const std::string& rendererImage, const std::string& mathImage);
    ~RopeSolver();
    RopeSolver(const RopeSolver&) = delete;
    RopeSolver& operator=(const RopeSolver&) = delete;
    void reset(unsigned state);
    void parameter(std::string_view name, float value);
    void step();
    void advance(double seconds);
    Nodes positions() const;
    float time() const;
    uint32_t randomState() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
