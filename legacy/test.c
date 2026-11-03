#include <stdio.h>

int main() {
    int x;
    scanf("%d", &x);
    
    int arr[x];
    for (int i = 0; i < x; i++) {
        arr[i] = i + 1;
    }
    for (int i = 0; i < x; i++) {
        printf("Elem nr %d is %d\n", i, arr[i]);
    }
    return 0;
}