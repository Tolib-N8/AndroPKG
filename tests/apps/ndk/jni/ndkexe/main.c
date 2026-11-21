// An Android executable the app runs with ProcessBuilder (like Puzzles' generator):
// echoes its arguments, reads a line from stdin, writes to stderr, exits with 3.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int counter;  // must start at 0 in every run (fresh process image)

int main(int argc, char** argv) {
    counter++;
    char line[128] = "";
    if (!fgets(line, sizeof line, stdin)) strcpy(line, "<eof>");
    line[strcspn(line, "\n")] = 0;
    printf("argc=%d arg1=%s stdin=%s counter=%d\n", argc, argc > 1 ? argv[1] : "", line, counter);
    fprintf(stderr, "to stderr\n");
    exit(3);
}
