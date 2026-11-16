#include <stdio.h>

int sum_squares(int n) {
    int total = 0;
    for (int i = 1; i <= n; i++) {
        total += i * i;
    }
    return total;
}

int main(void) {
    int ans = sum_squares(6);
    printf("PicoOS C Demo!\n");
    printf("sum_squares(6) = %d\n", ans);
    printf("ticks = %d ms\n", ticks());

    cls(rgb(8, 12, 24));
    rect(0, 0, 160, 18, rgb(0, 80, 140));
    text(8, 5, "PicoOS v0.4 C Demo", rgb(0, 255, 255), rgb(0, 80, 140));

    for (int i = 0; i < 6; i++) {
        int h = (i + 1) * 9;
        int c = rgb(40 + i * 40, 220 - i * 25, 160);
        rect(14 + i * 22, 86 - h, 16, h, c);
    }

    text(8, 96, "sum_sq(6) = 91", rgb(255, 255, 255), rgb(8, 12, 24));
    text(8, 110, "Compiled on RP2040!", rgb(0, 255, 128), rgb(8, 12, 24));
    return 0;
}
