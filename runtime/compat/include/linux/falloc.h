// andropkg: only constants are needed; fallocate() itself is provided by the compat layer.
#pragma once
#define FALLOC_FL_KEEP_SIZE 0x01
#define FALLOC_FL_PUNCH_HOLE 0x02
