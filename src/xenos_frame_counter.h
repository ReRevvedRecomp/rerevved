#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace rerevved::xenos_frames
{

inline constexpr int64_t  kSecondNs        = 1'000'000'000;
inline constexpr uint32_t kUnknownInterval = UINT32_MAX;

struct Bin
{
    int64_t                 startNs{};
    int64_t                 endNs{};
    uint64_t                attempts{};
    uint64_t                returns{};
    uint64_t                totalReturns{};
    double                  returnsPerSecond{};
    double                  dtMinMs{};
    double                  dtMeanMs{};
    double                  dtMaxMs{};
    std::array<uint64_t, 4> intervals{};
    bool                    partial{};
};

struct Summary
{
    int64_t  startNs{};
    int64_t  endNs{};
    uint64_t totalReturns{};
    uint64_t fullBins{};
    double   meanReturnsPerSecond{};
    double   minReturnsPerSecond{};
    double   maxReturnsPerSecond{};
};

// The caller serializes access. The injected clock supplies monotonic nanoseconds.
// Bins are [start, end); a gap belongs to the bin containing its later return.
class Counter
{
public:
    using Clock = int64_t (*)(void*);

    Counter(bool enabled, Clock clock, void* clockContext);
    void             Attempt();
    void             Return(uint32_t decodedInterval);
    void             Poll();
    void             Finish();
    std::vector<Bin> TakeBins();
    const Summary&   GetSummary() const;

private:
    void advance(int64_t now);
    void emit(int64_t end, bool partial);

    bool             enabled{};
    Clock            clock{};
    void*            clockContext{};
    Bin              current{};
    Summary          summary{};
    std::vector<Bin> completed;
    uint64_t         returnSequence{};
    bool             hasPreviousReturn{};
    int64_t          previousReturnNs{};
    uint64_t         dtCount{};
    int64_t          dtMinNs{};
    int64_t          dtSumNs{};
    int64_t          dtMaxNs{};
};

// Runtime integration lives in app.cpp; the statistics unit has no SDK dependency.
void Start();
void Finalize();
void RecordAttempt();
void RecordReturn(uint32_t decodedInterval);

} // namespace rerevved::xenos_frames
