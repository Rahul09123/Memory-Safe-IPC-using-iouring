import csv
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np

# Apply formal academic plotting aesthetics
sns.set_theme(style="whitegrid")
plt.rcParams.update({
    "font.size": 11,
    "axes.labelsize": 12,
    "axes.titlesize": 14,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "figure.titlesize": 16,
    "grid.color": "#e0e0e0"
})

# ============================================================================
# 1. EMPIRICAL DATA INGESTION (EXACT MATCH TO USER RUN LOGS)
# ============================================================================
sizes_list = [64, 256, 1024, 4096, 16384, 65536, 262144, 1048576]
labels_list = ['64B', '256B', '1KB', '4KB', '16KB', '64KB', '256KB', '1MB']
indices = np.arange(len(labels_list))

# Raw Throughput Allocations (GB/s)
pipe_tput = {
    64: [0.131223, 0.125580, 0.124381, 0.128461, 0.127851, 0.097051, 0.097328, 0.099064, 0.101749, 0.106757],
    256: [0.453827, 0.452682, 0.431951, 0.424858, 0.411787, 0.372908, 0.376603, 0.369816, 0.373452, 0.378799],
    1024: [1.225690, 1.189720, 1.200090, 1.179500, 1.169160, 1.068920, 1.099870, 1.086560, 1.104600, 1.088200],
    4096: [2.845110, 2.820900, 2.831630, 2.882790, 2.794330, 2.618090, 2.625480, 2.661430, 2.637200, 2.577070],
    16384: [3.880910, 4.128750, 3.866590, 4.141730, 3.792630, 3.886530, 3.761860, 3.765330, 3.637060, 3.731570],
    65536: [4.674390, 4.649420, 4.810660, 4.543090, 4.697460, 4.490100, 4.662560, 4.322670, 4.656790, 4.466330],
    262144: [4.820430, 5.101490, 4.774670, 4.804680, 4.936490, 4.788050, 4.575200, 4.752350, 4.598790, 4.658970],
    1048576: [4.800530, 4.675910, 4.608380, 4.812710, 4.529780, 4.307610, 4.555140, 4.285160, 4.505470, 4.508710]
}

uring_tput = {
    64: [0.495818, 0.496192, 0.494269, 0.496494, 0.490516, 0.579263, 0.577861, 0.572962, 0.578179, 0.573429],
    256: [1.888570, 1.929390, 1.902480, 1.912890, 1.790590, 1.752100, 1.810100, 1.845850, 1.874090, 1.882420],
    1024: [6.168610, 5.763810, 5.872800, 6.120500, 5.807590, 6.128850, 6.107990, 5.638580, 5.759510, 5.793460],
    4096: [12.105300, 12.067600, 12.389500, 11.036200, 12.089900, 11.813800, 11.512100, 10.991200, 11.221500, 11.104400],
    16384: [16.065300, 16.290700, 16.366400, 16.324000, 16.055300, 16.163200, 15.588800, 15.755100, 15.216600, 15.825100],
    65536: [17.495900, 17.180000, 17.188600, 17.076200, 16.863800, 16.089700, 16.109000, 16.256200, 16.194900, 16.225300],
    262144: [19.202500, 18.274100, 18.575700, 18.835100, 18.605000, 19.030800, 19.128100, 17.662800, 16.455900, 17.954500],
    1048576: [18.761000, 17.793300, 18.142600, 17.820300, 18.046600, 17.743700, 17.126500, 16.266900, 16.353100, 16.265400]
}

# Empirical Mean Latency Data Points (µs)
pipe_lat_means = [7121.21, 2246.51, 630.04, 231.25, 214.34, 191.31, 175.03, 262.47]
uring_lat_means = [7.40, 6.45, 2.88, 1.75, 56.12, 224.28, 811.85, 3313.43]

# Percentile Boundaries for Microarchitectural Tail Profiling (p50, p99)
pipe_p50 = [7138.20, 2221.75, 615.11, 224.27, 204.43, 181.93, 168.02, 255.49]
pipe_p99 = [9747.21, 3217.30, 915.22, 413.48, 381.16, 385.22, 368.51, 717.91]

uring_p50 = [6.96, 6.69, 2.37, 1.11, 56.23, 228.41, 807.69, 3385.12]
uring_p99 = [17.12, 13.56, 10.74, 17.55, 101.40, 308.87, 1530.65, 5133.55]

# ============================================================================
# 2. FILE MANAGEMENT SYSTEM: EXPORT SEPARATE CSV RUN LOGS
# ============================================================================
with open('normal_pipe_runs.csv', mode='w', newline='') as p_file:
    writer = csv.writer(p_file)
    writer.writerow(['message_size_bytes', 'run_number', 'throughput_gbps'])
    for sz in sizes_list:
        for idx, val in enumerate(pipe_tput[sz]):
            writer.writerow([sz, idx + 1, round(val, 6)])

with open('io_uring_runs.csv', mode='w', newline='') as u_file:
    writer = csv.writer(u_file)
    writer.writerow(['message_size_bytes', 'run_number', 'throughput_gbps'])
    for sz in sizes_list:
        for idx, val in enumerate(uring_tput[sz]):
            writer.writerow([sz, idx + 1, round(val, 6)])

print("[✓] Generated independent run vectors: 'normal_pipe_runs.csv' and 'io_uring_runs.csv'")

# Compute Means for Plotting
pipe_mean_tput = [np.mean(pipe_tput[sz]) for sz in sizes_list]
uring_mean_tput = [np.mean(uring_tput[sz]) for sz in sizes_list]

# ============================================================================
# VISUALIZATION 1: THROUGHPUT PERFORMANCE COMPARISON
# ============================================================================
plt.figure(figsize=(9, 5.5))
bar_width = 0.35

plt.bar(indices - bar_width/2, pipe_mean_tput, bar_width, label='Standard POSIX Pipe', color='#e74c3c', edgecolor='#c0392b', alpha=0.85)
plt.bar(indices + bar_width/2, uring_mean_tput, bar_width, label='io_uring + Shared Ring', color='#2ecc71', edgecolor='#27ae60', alpha=0.85)

for i in range(len(labels_list)):
    factor = uring_mean_tput[i] / pipe_mean_tput[i]
    plt.text(indices[i], max(pipe_mean_tput[i], uring_mean_tput[i]) + 0.4, f"{factor:.2f}x", ha='center', fontweight='bold', color='#2c3e50', fontsize=9.5)

plt.title('Multi-Run Aggregated IPC Throughput Scaling Comparison\n(Higher is Better)', fontsize=13, pad=15)
plt.xlabel('Message Payload Block Size', fontsize=11, labelpad=8)
plt.ylabel('Averaged Throughput (GB/s)', fontsize=11, labelpad=8)
plt.xticks(indices, labels_list)
plt.ylim(0, max(uring_mean_tput) + 3)
plt.legend(loc='upper left', frameon=True, facecolor='white')
plt.tight_layout()
plt.savefig('1_throughput_scaling.png', dpi=300)
plt.close()

# ============================================================================
# VISUALIZATION 2: LATENCY TRAJECTORY & TAIL SPECTRUM (LOGARITHMIC)
# ============================================================================
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6), sharey=False)

# Panel A: POSIX Pipe Latency Percentile Spread
ax1.plot(labels_list, pipe_lat_means, label='Mean Latency', color='#c0392b', marker='o', linewidth=2)
ax1.plot(labels_list, pipe_p50, label='p50 Percentile', color='#e67e22', marker='v', linestyle=':')
ax1.plot(labels_list, pipe_p99, label='p99 Percentile', color='#9b59b6', marker='^', linestyle='--')
ax1.set_yscale('log')
ax1.set_title('POSIX Pipe Latency Profile Spectrum', fontsize=12, pad=10)
ax1.set_xlabel('Payload Block Size')
ax1.set_ylabel('Execution Latency Time (µs)')
ax1.legend(loc='lower left', frameon=True)

# Panel B: io_uring Latency Percentile Spread
ax2.plot(labels_list, uring_lat_means, label='Mean Latency', color='#27ae60', marker='s', linewidth=2)
ax2.plot(labels_list, uring_p50, label='p50 Percentile', color='#f1c40f', marker='v', linestyle=':')
ax2.plot(labels_list, uring_p99, label='p99 Percentile', color='#2980b9', marker='^', linestyle='--')
ax2.set_yscale('log')
ax2.set_title('io_uring Shared Ring Latency Profile Spectrum', fontsize=12, pad=10)
ax2.set_xlabel('Payload Block Size')
ax2.legend(loc='upper left', frameon=True)

plt.suptitle('IPC Latency Footprint and Percentile Distribution Analysis\n(Logarithmic Scale - Lower is Better)', y=0.98, fontsize=14)
plt.tight_layout()
plt.savefig('2_latency_profiles.png', dpi=300)
plt.close()

# ============================================================================
# VISUALIZATION 3: OPERATIONAL EFFICIENCY SPECTRUM (AMORTIZATION)
# ============================================================================
# Calculates transactions/msg ops processed per millisecond based on throughput data
pipe_ops_per_ms = [(pipe_mean_tput[i] * 1024 * 1024 * 1024) / (sizes_list[i] * 1000) for i in range(len(sizes_list))]
uring_ops_per_ms = [(uring_mean_tput[i] * 1024 * 1024 * 1024) / (sizes_list[i] * 1000) for i in range(len(sizes_list))]

plt.figure(figsize=(9, 5.5))
plt.plot(labels_list, pipe_ops_per_ms, marker='o', color='#e74c3c', linewidth=2.5, label='Standard POSIX Pipe', linestyle='--')
plt.plot(labels_list, uring_ops_per_ms, marker='s', color='#2ecc71', linewidth=2.5, label='io_uring + Shared Ring')

plt.yscale('log')
plt.title('Operational Transaction Density Processing Capacity\n(Message Operations Completed per Millisecond - Higher is Better)', fontsize=12, pad=15)
plt.xlabel('Message Payload Block Size', fontsize=11)
plt.ylabel('Operations Synchronized / Millisecond (Log Scale)', fontsize=11)
plt.grid(True, which="both", linestyle="--", alpha=0.6)
plt.legend(loc='upper right', frameon=True, facecolor='white')
plt.tight_layout()
plt.savefig('3_operational_efficiency.png', dpi=300)
plt.close()

print("[✓] Successfully finalized and saved all three custom academic visualizations.")