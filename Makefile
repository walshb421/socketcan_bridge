CC      := gcc
CFLAGS  := -Wall -Wextra -g -I inc
LDFLAGS :=

LIB_CFLAGS  := -Wall -Wextra -g -fPIC
LIB_LDFLAGS := -shared

LIB_DIR := lib
SRC_DIR := bridge
OBJ_DIR := build
BIN_DIR := bin
INC_DIR := $(BIN_DIR)/inc


LIB_NAMES	:= $(notdir $(wildcard $(LIB_DIR)/*))
TARGET  := $(BIN_DIR)/socketcan_bridge

SRCS    := $(wildcard $(SRC_DIR)/*.c)
HDRS		:= $(wildcard $(SRC_DIR)/*.h)
OBJS    := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/$(SRC_DIR)/%.o, $(SRCS))
INCS    := $(patsubst $(SRC_DIR)/%.h, $(INC_DIR)/%.h, $(HDRS))
DEPS    := $(OBJS:.o=.d)

-include $(DEPS)


LIBS := $(foreach lib, $(LIB_NAMES), $(BIN_DIR)/$(lib).so)

all: $(LIBS)



$(BIN_DIR)/%.so:
	touch $@

#$(TARGET): $(OBJS) | $(BIN_DIR)
#	$(CC) $(LDFLAGS) $^ -o $@


# Object files depend on the stamp, not the headers directly
#$(OBJ_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(STAMP) | $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR)
#	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

#$(BIN_DIR) $(OBJ_DIR) $(OBJ_DIR)/$(SRC_DIR):
#	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all clean
