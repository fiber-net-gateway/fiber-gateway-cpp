#include <iostream>
#include <expected>


template<typename T>
    requires requires(T x) {
        { x + x } -> std::convertible_to<T>;
    }
T add(T a, T b) {
    return a + b;
}


int main() {
    std::string t = add(std::string("a"), std::string("b"));
    std::cout << "Hello World!" << t << std::endl;
    return 0;
}
