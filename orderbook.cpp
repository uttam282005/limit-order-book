#include <iostream>
#include <unordered_map>
#include <map>
#include <list>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <algorithm>
#include <vector>
#include <thread>
#include <chrono>
#include <random>

enum class Side { BUY, SELL };

struct Order {
    std::string order_id;
    uint64_t timestamp;
    Side side;
    double price;
    int volume;
    std::string client_name;
};

class OrderBook {
private:
    std::map<double, std::list<std::shared_ptr<Order>>> asks;
    std::map<double, std::list<std::shared_ptr<Order>>, std::greater<double>> bids;
    std::unordered_map<std::string, std::list<std::shared_ptr<Order>>::iterator> order_map;
    std::unordered_map<double, int> ask_volume_map;
    std::unordered_map<double, int> bid_volume_map;
    mutable std::shared_mutex rw_lock;

public:
    void placeOrder(std::shared_ptr<Order> order) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        if (order->side == Side::BUY) {
            matchOrder(order, asks, ask_volume_map);
            if (order->volume > 0) addOrderToBook(order, bids, bid_volume_map);
        } else {
            matchOrder(order, bids, bid_volume_map);
            if (order->volume > 0) addOrderToBook(order, asks, ask_volume_map);
        }
    }

    void cancelOrder(const std::string& order_id) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        auto it = order_map.find(order_id);
        if (it == order_map.end()) return; 

        std::shared_ptr<Order> order = *it->second;
        if (order->side == Side::BUY) {
            bid_volume_map[order->price] -= order->volume;
            bids[order->price].erase(it->second);
            if (bids[order->price].empty()) bids.erase(order->price);
        } else {
            ask_volume_map[order->price] -= order->volume;
            asks[order->price].erase(it->second);
            if (asks[order->price].empty()) asks.erase(order->price);
        }
        order_map.erase(it);
    }

private:
    template<typename BookType>
    void matchOrder(std::shared_ptr<Order>& order, BookType& opposite_book, std::unordered_map<double, int>& opposite_volume_map) {
        while (order->volume > 0 && !opposite_book.empty()) {
            auto best_price_level = opposite_book.begin();
            double best_price = best_price_level->first;

            if ((order->side == Side::BUY && order->price < best_price) ||
                (order->side == Side::SELL && order->price > best_price)) {
                break; 
            }

            auto& order_queue = best_price_level->second;
            while (order->volume > 0 && !order_queue.empty()) {
                auto matched_order = order_queue.front();
                int trade_volume = std::min(order->volume, matched_order->volume);

                // std::cout << "TRADE EXECUTED...\n"; // REMOVED FOR BENCHMARKING

                order->volume -= trade_volume;
                matched_order->volume -= trade_volume;
                opposite_volume_map[best_price] -= trade_volume;

                if (matched_order->volume == 0) {
                    order_map.erase(matched_order->order_id);
                    order_queue.pop_front();
                }
            }
            if (order_queue.empty()) opposite_book.erase(best_price_level);
        }
    }

    template<typename BookType>
    void addOrderToBook(std::shared_ptr<Order> order, BookType& book, std::unordered_map<double, int>& volume_map) {
        book[order->price].push_back(order);
        auto it = std::prev(book[order->price].end());
        order_map[order->order_id] = it;
        volume_map[order->price] += order->volume;
    }
};

// ==========================================
// BENCHMARKING SUITE
// ==========================================

void simulateTrading(OrderBook& book, int thread_id, int num_orders) {
    // Independent random engine per thread to avoid locking random state
    std::mt19937 rng(1337 + thread_id);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<double> price_dist(95.0, 105.0); // Tightly grouped to force matches
    std::uniform_int_distribution<int> vol_dist(10, 100);

    for (int i = 0; i < num_orders; ++i) {
        auto order = std::make_shared<Order>();
        order->order_id = "T" + std::to_string(thread_id) + "_O" + std::to_string(i);
        order->timestamp = i;
        order->side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;

        // Round to nearest 0.01 (standard tick size)
        order->price = std::round(price_dist(rng) * 100.0) / 100.0;
        order->volume = vol_dist(rng);
        order->client_name = "Algo_" + std::to_string(thread_id);

        book.placeOrder(order);
    }
}

int main() {
    OrderBook book;

    const int num_threads = 8;
    const int orders_per_thread = 250000;
    const int total_orders = num_threads * orders_per_thread;

    std::cout << "Starting Benchmark...\n";
    std::cout << "Threads: " << num_threads << "\n";
    std::cout << "Total Orders: " << total_orders << "\n";
    std::cout << "-----------------------------------\n";

    std::vector<std::thread> workers;

    // Start High-Resolution Timer
    auto start_time = std::chrono::high_resolution_clock::now();

    // Launch Threads
    for (int i = 0; i < num_threads; ++i) {
        workers.emplace_back(simulateTrading, std::ref(book), i, orders_per_thread);
    }

    // Wait for all threads to finish
    for (auto& t : workers) {
        t.join();
    }

    // Stop Timer
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    // Calculate Metrics
    double seconds = elapsed.count();
    double throughput = total_orders / seconds;

    std::cout << "Execution Time: " << seconds << " seconds\n";
    std::cout << "Throughput:     " << throughput << " orders/second\n";
    std::cout << "-----------------------------------\n";

    return 0;
}
