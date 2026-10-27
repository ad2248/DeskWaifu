// Deliberately invalid *test fixture*: a recoverable UBSan diagnostic must not
// turn an otherwise successful process into a green regression gate.
#include <climits>
int main()
{
    volatile int maximum = INT_MAX;
    const int overflow = maximum + 1;
    return overflow == 0;
}
