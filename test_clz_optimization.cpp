// Test to verify CLZ optimization for lossless JPEG encoding
// Compare table lookup vs __builtin_clz performance

#include <chrono>
#include <iostream>
#include <random>

// Old table-based approach
static int numBitsTable[256];

void init_table() {
    numBitsTable[0] = 0;
    for (int i = 1; i < 256; i++) {
        int temp = i;
        int nbits = 1;
        while (temp >>= 1) {
            nbits++;
        }
        numBitsTable[i] = nbits;
    }
}

inline int get_nbits_table(int diff) {
    int temp = diff < 0 ? -diff : diff;
    return temp >= 256 ? numBitsTable[temp >> 8] + 8
                       : numBitsTable[temp & 0xFF];
}

// New CLZ-based approach
inline int get_nbits_clz(int diff) {
    int temp = diff < 0 ? -diff : diff;
    return temp ? (32 - __builtin_clz(static_cast<unsigned>(temp))) : 0;
}

int main() {
    init_table();
    
    // Generate test data (typical JPEG prediction differences: -4096 to +4096)
    constexpr int NUM_SAMPLES = 10000000;
    std::vector<int> test_data(NUM_SAMPLES);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(-4096, 4096);
    
    for (int i = 0; i < NUM_SAMPLES; i++) {
        test_data[i] = dist(rng);
    }
    
    // Benchmark table lookup
    auto start = std::chrono::high_resolution_clock::now();
    volatile int sum1 = 0;
    for (int val : test_data) {
        sum1 += get_nbits_table(val);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto table_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    // Benchmark CLZ
    start = std::chrono::high_resolution_clock::now();
    volatile int sum2 = 0;
    for (int val : test_data) {
        sum2 += get_nbits_clz(val);
    }
    end = std::chrono::high_resolution_clock::now();
    auto clz_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    // Verify correctness
    bool correct = true;
    for (int i = 0; i < 1000; i++) {
        if (get_nbits_table(test_data[i]) != get_nbits_clz(test_data[i])) {
            std::cerr << "Mismatch at " << test_data[i] << std::endl;
            correct = false;
            break;
        }
    }
    
    std::cout << "=== CLZ Optimization Benchmark ===" << std::endl;
    std::cout << "Samples: " << NUM_SAMPLES << std::endl;
    std::cout << "Table lookup: " << table_ns / 1e6 << " ms ("
              << static_cast<double>(table_ns) / NUM_SAMPLES << " ns/op)" << std::endl;
    std::cout << "CLZ intrinsic: " << clz_ns / 1e6 << " ms ("
              << static_cast<double>(clz_ns) / NUM_SAMPLES << " ns/op)" << std::endl;
    std::cout << "Speedup: " << static_cast<double>(table_ns) / clz_ns << "x" << std::endl;
    std::cout << "Correctness: " << (correct ? "PASS" : "FAIL") << std::endl;
    
    return 0;
}
