CXX = g++

CXXFLAGS = -O3 -g -fno-omit-frame-pointer \
           -march=native -pthread

LIBS = -luring

FLAMEGRAPH_DIR = $(HOME)/FlameGraph

# =========================================================
# Targets
# =========================================================

all: normal_pipe shared_ring_ipc

# =========================================================
# Build
# =========================================================

normal_pipe: normal_pipe.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

shared_ring_ipc: shared_ring_ipc.cpp
	$(CXX) $(CXXFLAGS) $< $(LIBS) -o $@

# =========================================================
# Run Benchmarks
# =========================================================

run-normal:
	./normal_pipe

run-ring:
	./shared_ring_ipc

# =========================================================
# perf Recording
# =========================================================

perf-normal:
	sudo perf record \
		--call-graph dwarf \
		-F 999 \
		-o normal.perf.data \
		./normal_pipe

	sudo chown $(USER):$(USER) normal.perf.data

perf-ring:
	sudo perf record \
		--call-graph dwarf \
		-F 999 \
		-o ring.perf.data \
		./shared_ring_ipc

	sudo chown $(USER):$(USER) ring.perf.data

# =========================================================
# Flamegraphs
# =========================================================

flame-normal:
	perf script -i normal.perf.data > normal.perf

	$(FLAMEGRAPH_DIR)/stackcollapse-perf.pl \
		normal.perf > normal.folded

	$(FLAMEGRAPH_DIR)/flamegraph.pl \
		--title "Normal Pipe IPC" \
		normal.folded > normal.svg

flame-ring:
	perf script -i ring.perf.data > ring.perf

	$(FLAMEGRAPH_DIR)/stackcollapse-perf.pl \
		ring.perf > ring.folded

	$(FLAMEGRAPH_DIR)/flamegraph.pl \
		--title "Shared Ring IPC" \
		ring.folded > ring.svg

# =========================================================
# Full Profiling Pipelines
# =========================================================

profile-normal: perf-normal flame-normal

profile-ring: perf-ring flame-ring

# =========================================================
# perf stat Benchmarks
# =========================================================

stat-normal:
	perf stat -e \
		cache-misses,\
		cache-references,\
		context-switches,\
		cycles,\
		instructions \
		./normal_pipe

stat-ring:
	perf stat -e \
		cache-misses,\
		cache-references,\
		context-switches,\
		cycles,\
		instructions \
		./shared_ring_ipc

# =========================================================
# Open Flamegraphs
# =========================================================

open-normal:
	firefox normal.svg

open-ring:
	firefox ring.svg

# =========================================================
# Clean
# =========================================================

clean:
	rm -f \
		normal_pipe \
		shared_ring_ipc \
		*.perf \
		*.folded \
		*.svg \
		*.perf.data \
		*.csv