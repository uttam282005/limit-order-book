#include <iostream>
#include <vector>
#include <map>
#include <list>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>

enum class Side { BUY, SELL };

struct Order {
    uint64_t order_id;
    uint64_t timestamp;
    Side side;
    int64_t price_ticks;
    int volume;
    uint32_t client_id;
};

template <typename T, size_t PoolSize>
class MemoryPool {
private:
    std::vector<T> pool;
    std::vector<T*> free_list;
    std::mutex lock;

public:
    MemoryPool() : pool(PoolSize) {
        free_list.reserve(PoolSize);
        for (size_t i = 0; i < PoolSize; ++i) {
            free_list.push_back(&pool[i]);
        }
    }

    T* allocate() {
        std::lock_guard<std::mutex> lk(lock);
        if (free_list.empty()) return nullptr; // Out of memory
        T* obj = free_list.back();
        free_list.pop_back();
        return obj;
    }

    void deallocate(T* obj) {
        std::lock_guard<std::mutex> lk(lock);
        free_list.push_back(obj);
    }
};

class OrderBook {
private:
    std::map<int64_t, std::list<Order*>> asks;
    std::map<int64_t, std::list<Order*>, std::greater<int64_t>> bids;
    
    std::unordered_map<uint64_t, std::list<Order*>::iterator> order_map;
    std::unordered_map<int64_t, int> ask_volume_map;
    std::unordered_map<int64_t, int> bid_volume_map;
    
    mutable std::shared_mutex rw_lock;
    MemoryPool<Order, 1000000>& order_pool;

public:
    OrderBook(MemoryPool<Order, 1000000>& pool) : order_pool(pool) {}

    void placeOrder(Order* order) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        if (order->side == Side::BUY) {
            matchOrder(order, asks, ask_volume_map);
            if (order->volume > 0) addOrderToBook(order, bids, bid_volume_map);
            else order_pool.deallocate(order); // Return memory if fully filled instantly
        } else {
            matchOrder(order, bids, bid_volume_map);
            if (order->volume > 0) addOrderToBook(order, asks, ask_volume_map);
            else order_pool.deallocate(order);
        }
    }

    void cancelOrder(uint64_t order_id) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        auto it = order_map.find(order_id);
        if (it == order_map.end()) return; 

        Order* order = *it->second;
        if (order->side == Side::BUY) {
            bid_volume_map[order->price_ticks] -= order->volume;
            bids[order->price_ticks].erase(it->second);
            if (bids[order->price_ticks].empty()) bids.erase(order->price_ticks);
        } else {
            ask_volume_map[order->price_ticks] -= order->volume;
            asks[order->price_ticks].erase(it->second);
            if (asks[order->price_ticks].empty()) asks.erase(order->price_ticks);
        }

        order_map.erase(it);
        order_pool.deallocate(order);
    }

private:
    template<typename BookType>
    void matchOrder(Order* order, BookType& opposite_book, std::unordered_map<int64_t, int>& opposite_volume_map) {
        while (order->volume > 0 && !opposite_book.empty()) {
            auto best_price_level = opposite_book.begin();
            int64_t best_price = best_price_level->first;

            if ((order->side == Side::BUY && order->price_ticks < best_price) ||
                (order->side == Side::SELL && order->price_ticks > best_price)) {
                break; 
            }

            auto& order_queue = best_price_level->second;
            while (order->volume > 0 && !order_queue.empty()) {
                Order* matched_order = order_queue.front();
                int trade_volume = std::min(order->volume, matched_order->volume);

                order->volume -= trade_volume;
                matched_order->volume -= trade_volume;
                opposite_volume_map[best_price] -= trade_volume;

                if (matched_order->volume == 0) {
                    order_map.erase(matched_order->order_id);
                    order_queue.pop_front();
                    order_pool.deallocate(matched_order); // Recycle memory
                }
            }
            if (order_queue.empty()) opposite_book.erase(best_price_level);
        }
    }

    template<typename BookType>
    void addOrderToBook(Order* order, BookType& book, std::unordered_map<int64_t, int>& volume_map) {
        book[order->price_ticks].push_back(order);
        auto it = std::prev(book[order->price_ticks].end());
        order_map[order->order_id] = it;
        volume_map[order->price_ticks] += order->volume;
    }
};

// ==========================================
// BENCHMARKING SUITE (LATENCY PROFILING)
// ==========================================

void simulateTrading(OrderBook& book, MemoryPool<Order, 1000000>& pool, int thread_id, int num_orders, std::vector<double>& latencies) {
    std::mt19937 rng(1337 + thread_id);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> price_dist(9950, 10050);
    std::uniform_int_distribution<int> vol_dist(10, 100);

    for (int i = 0; i < num_orders; ++i) {
        Order* order = pool.allocate();
        if (!order) continue;

        order->order_id = ((uint64_t)thread_id << 32) | i;
        order->timestamp = i;
        order->side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        order->price_ticks = price_dist(rng);
        order->volume = vol_dist(rng);
        order->client_id = thread_id;

        // Measure Microsecond Latency for placeOrder
        auto start = std::chrono::high_resolution_clock::now();
        
        book.placeOrder(order);
        
        auto end = std::chrono::high_resolution_clock::now();
        double micro_secs = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(micro_secs);
    }
}

int main() {
    auto global_pool = std::make_unique<MemoryPool<Order, 1000000>>();
    
    OrderBook book(*global_pool);
    
    const int num_threads = 4;
    const int orders_per_thread = 100000;

    std::cout << "Starting HFT Benchmark...\n";
    std::vector<std::thread> workers;
    std::vector<std::vector<double>> thread_latencies(num_threads);

    for(int i = 0; i < num_threads; ++i) {
        thread_latencies[i].reserve(orders_per_thread);
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        workers.emplace_back(simulateTrading, std::ref(book), std::ref(*global_pool), i, orders_per_thread, std::ref(thread_latencies[i]));
    }

    for (auto& t : workers) t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_seconds = std::chrono::duration<double>(end_time - start_time).count();

    // Aggregate latencies to calculate p99
    std::vector<double> all_latencies;
    for(const auto& lats : thread_latencies) {
        all_latencies.insert(all_latencies.end(), lats.begin(), lats.end());
    }
    std::sort(all_latencies.begin(), all_latencies.end());

    double p50 = all_latencies[all_latencies.size() * 0.50];
    double p99 = all_latencies[all_latencies.size() * 0.99];
    double p999 = all_latencies[all_latencies.size() * 0.999];

    std::cout << "Total Orders: " << all_latencies.size() << "\n";
    std::cout << "Throughput:   " << all_latencies.size() / total_seconds << " ops/sec\n";
    std::cout << "--- Latency Profiling ---\n";
    std::cout << "Median (p50): " << p50 << " microseconds\n";
    std::cout << "p99 Latency:  " << p99 << " microseconds\n";
    std::cout << "p99.9 Latency:" << p999 << " microseconds\n";

    return 0;
}
