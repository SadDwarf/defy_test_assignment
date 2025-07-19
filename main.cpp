#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sched.h>
#include <thread>
#include <vector>

constexpr size_t kCacheLineLen = 64;

void set_affinity(const int num)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(num, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
    {
        perror("sched_setaffinity");
        exit(1);
    }
}

class SyncState
{
public:
    enum State : uint8_t
    {
        Empty = 0,
        ReaderReady = 1,
        MessageSent = 2,
    };

    SyncState() { flag.store(Empty); };

    void spin_exchange(const State from, const State to)
    {
        while (true)
        {
            State tmp = from;
            if (flag.compare_exchange_strong(tmp, to, std::memory_order_acquire))
            {
                break;
            }
            while (flag.load(std::memory_order_relaxed) != from)
            {
            }
        }
    }

    void wait(const State state) const
    {
        while (flag.load(std::memory_order_relaxed) != state)
        {
        }
    }

    std::atomic<State> flag;
};

struct alignas(kCacheLineLen) SingleCacheLineMessage
{
    char data[kCacheLineLen];
};

struct alignas(kCacheLineLen) DoubleCacheLineMessage
{
    char data[2 * kCacheLineLen];
};

template <typename T>
struct SharedState
{
    T msg;
    SyncState sync;
};

template <typename T>
T generate_message()
{
    T msg{};
    for (char& byte : msg.data)
    {
        byte = static_cast<char>(std::rand() % 256);
    }
    return msg;
}

template <typename T>
void writer(const std::shared_ptr<SharedState<T>> state, const size_t iterations, const int cpu_id,
            std::vector<timespec>& times)
{
    timespec time;
    set_affinity(cpu_id);
    for (size_t i = 0; i < iterations; ++i)
    {
        T msg = generate_message<T>();
        state->sync.wait(SyncState::State::ReaderReady);
        clock_gettime(CLOCK_MONOTONIC_RAW, &time);
        std::memcpy(state->msg.data, &msg.data, sizeof(msg.data));
        state->sync.spin_exchange(SyncState::State::ReaderReady, SyncState::State::MessageSent);
        times[i] = time;
    }
}

// Used by reader to avoid situations when read operations are optimized out by the compiler.
DoubleCacheLineMessage out;

template <typename T>
void reader(const std::shared_ptr<SharedState<T>> state, size_t iterations, int cpu_id, std::vector<timespec>& times)
{
    timespec time;
    set_affinity(cpu_id);
    // MessageSent initial state used as optimization technique to avoid comparing with Empty in loop.
    state->sync.spin_exchange(SyncState::State::Empty, SyncState::State::MessageSent);
    for (size_t i = 0; i < iterations; ++i)
    {
        state->sync.spin_exchange(SyncState::State::MessageSent, SyncState::State::ReaderReady);
        state->sync.wait(SyncState::State::MessageSent);
        std::memcpy(out.data, &state->msg.data, sizeof(state->msg.data));
        clock_gettime(CLOCK_MONOTONIC_RAW, &time);

        times[i] = time;
    }
}

struct BenchmarkResult
{
    size_t median;
    size_t p90;
    size_t p95;
};

void print_results(const std::vector<std::vector<BenchmarkResult>>& results)
{
    std::cout << "Benchmark results (median/p90/p95) in nanoseconds:\nfrom\\to ";
    for (size_t i = 0; i < results.size(); ++i)
    {
        std::cout << std::setw(14) << i << "   ";
    }
    std::cout << std::endl;
    for (size_t i = 0; i < results.size(); ++i)
    {
        std::cout << std::setw(7) << i << " ";
        ;
        for (size_t j = 0; j < results[i].size(); ++j)
        {
            BenchmarkResult res;
            if (i == j)
            {
                res = BenchmarkResult{0, 0, 0};
            }
            else
            {
                res = results[i][j];
            }
            std::cout << std::format("{:>4}/{:>4}/{:>4}   ", res.median, res.p90, res.p95);
        }
        std::cout << std::endl;
    }
}

BenchmarkResult calculate_timings(const std::vector<timespec>& send_times, const std::vector<timespec>& receive_times)
{
    static constexpr uint64_t kNanosInSec = 1'000'000'000;
    std::vector<uint64_t> durations(send_times.size());
    for (size_t i = 0; i < durations.size(); ++i)
    {

        durations[i] = (receive_times[i].tv_sec - send_times[i].tv_sec) * kNanosInSec +
            (receive_times[i].tv_nsec - send_times[i].tv_nsec);
    }
    std::sort(durations.begin(), durations.end());
    return BenchmarkResult{.median = durations[durations.size() / 2],
                           .p90 = durations[durations.size() * 90 / 100],
                           .p95 = durations[durations.size() * 95 / 100]};
}

template <typename T>
void benchmark(const size_t num_threads, const size_t iterations)
{
    std::vector<timespec> send_times(iterations);
    std::vector<timespec> receive_times(iterations);

    std::vector results(num_threads, std::vector<BenchmarkResult>(num_threads));

    for (size_t reader_cpu = 0; reader_cpu < num_threads; ++reader_cpu)
    {
        for (size_t writer_cpu = 0; writer_cpu < num_threads; ++writer_cpu)
        {
            if (reader_cpu == writer_cpu)
            {
                continue;
            }
            std::shared_ptr<SharedState<T>> state = std::make_shared<SharedState<T>>();

            std::thread writer_thread(writer<T>, state, iterations, writer_cpu, std::ref(send_times));
            std::thread reader_thread(reader<T>, state, iterations, reader_cpu, std::ref(receive_times));

            writer_thread.join();
            reader_thread.join();


            results[writer_cpu][reader_cpu] = calculate_timings(send_times, receive_times);
        }
    }

    print_results(results);
}

int main()
{
    constexpr size_t iterations = 10000;

    // Generate seed for rand().
    // Used only for generating message data. Problems with randomness doesn't really matter here.
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    size_t num_threads = std::thread::hardware_concurrency();

    std::cout << "Running benchmark sending SINGLE cache line message between CPUs" << std::endl;
    benchmark<SingleCacheLineMessage>(num_threads, iterations);

    std::cout << "\nRunning benchmark sending DOUBLE cache line message between CPUs" << std::endl;
    benchmark<DoubleCacheLineMessage>(num_threads, iterations);


    return 0;
}
