#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // For sleep on Unix-like systems

// --- 常量定义 ---
#define USER_MEMORY_SIZE 1024           // 用户内存大小，单位：K
#define TIME_QUANTUM 3                  // 时间片大小
#define MAX_JOBS 20                     // 后备队列最大作业数
#define MAX_PROCESSES_IN_MEMORY 5       // 内存中最多允许的进程数

// 进程状态
typedef enum { JOB_READY, READY, RUNNING, BLOCKED, TERMINATED } ProcessState;

// --- 数据结构 ---

// 进程结构体
typedef struct {
    int pid;                // 进程ID
    int job_id;             // 对应作业ID
    ProcessState state;     // 状态
    int required_cpu_time;  // 总CPU时间
    int current_cpu_time;   // 已执行CPU时间
    int memory_size;        // 内存大小
    int memory_start;       // 内存起始地址
    int time_slice_used;    // 当前时间片已用
    long block_start_time;  // 阻塞开始时间
    int block_duration;     // 阻塞时长
} Process;

// 内存块结构体
typedef struct {
    int start_address;      // 起始地址
    int size;               // 大小
    int is_free;            // 1空闲，0占用
    int owner_pid;          // 拥有者PID
} MemoryBlock;

// --- 全局变量 ---
long current_time = 0;
int next_pid = 0;
int next_job_id = 0;

// 内存管理
MemoryBlock memory_map[MAX_PROCESSES_IN_MEMORY * 2 + 2]; // 足够存储分裂后的块
int num_memory_blocks = 0;

// 队列
Process* job_queue[MAX_JOBS];
int job_queue_count = 0;
Process* ready_queue[MAX_PROCESSES_IN_MEMORY + 1]; // 循环队列
int ready_queue_head = 0;
int ready_queue_tail = 0;
int ready_queue_count = 0;
Process* blocked_queue[MAX_PROCESSES_IN_MEMORY + 1];
int blocked_queue_count = 0;

Process* running_process = NULL;
Process* completed_jobs[MAX_JOBS]; // 假设最多完成MAX_JOBS个作业
int completed_jobs_count = 0;

// --- 辅助函数声明 ---
void enqueue_job(Process* p);
Process* dequeue_job();
void enqueue_ready(Process* p);
Process* dequeue_ready();
void enqueue_blocked(Process* p);
void remove_from_blocked_queue(int pid);

// --- 内存管理 ---
void init_os() {
    printf("--- 操作系统模拟器启动 ---\n");
    memory_map[0].start_address = 0;
    memory_map[0].size = USER_MEMORY_SIZE;
    memory_map[0].is_free = 1;
    memory_map[0].owner_pid = -1;
    num_memory_blocks = 1;
    srand((unsigned int)time(NULL));
}

// 首次适应分配内存
int allocate_memory(int pid, int requested_size) {
    for (int i = 0; i < num_memory_blocks; i++) {
        if (memory_map[i].is_free && memory_map[i].size >= requested_size) {
            int allocated_start = memory_map[i].start_address;
            if (memory_map[i].size == requested_size) { // 刚好适配
                memory_map[i].is_free = 0;
                memory_map[i].owner_pid = pid;
            } else { // 分裂空闲块
                for (int j = num_memory_blocks; j > i + 1; j--) memory_map[j] = memory_map[j - 1];
                memory_map[i + 1].start_address = allocated_start + requested_size;
                memory_map[i + 1].size = memory_map[i].size - requested_size;
                memory_map[i + 1].is_free = 1;
                memory_map[i + 1].owner_pid = -1;
                num_memory_blocks++;
                memory_map[i].is_free = 0;
                memory_map[i].owner_pid = pid;
                memory_map[i].size = requested_size;
            }
            return allocated_start;
        }
    }
    return -1; // 分配失败
}

// 释放内存并合并空闲块
void free_memory(int pid) {
    int freed_idx = -1;
    for (int i = 0; i < num_memory_blocks; i++) {
        if (!memory_map[i].is_free && memory_map[i].owner_pid == pid) {
            memory_map[i].is_free = 1;
            memory_map[i].owner_pid = -1;
            freed_idx = i;
            break;
        }
    }
    if (freed_idx == -1) return;

    // 合并前一个块
    if (freed_idx > 0 && memory_map[freed_idx - 1].is_free) {
        memory_map[freed_idx - 1].size += memory_map[freed_idx].size;
        for (int i = freed_idx; i < num_memory_blocks - 1; i++) memory_map[i] = memory_map[i + 1];
        num_memory_blocks--;
        freed_idx--;
    }
    // 合并后一个块
    if (freed_idx < num_memory_blocks - 1 && memory_map[freed_idx + 1].is_free) {
        memory_map[freed_idx].size += memory_map[freed_idx + 1].size;
        for (int i = freed_idx + 1; i < num_memory_blocks - 1; i++) memory_map[i] = memory_map[i + 1];
        num_memory_blocks--;
    }
    printf("  [内存管理]: 进程 %d 内存已释放。\n", pid);
}

// --- 队列管理 ---
void enqueue_job(Process* p) {
    if (job_queue_count < MAX_JOBS) job_queue[job_queue_count++] = p;
    else { free(p); printf("  [作业调度]: 后备队列已满。\n"); }
}

Process* dequeue_job() {
    if (job_queue_count == 0) return NULL;
    Process* p = job_queue[0];
    for (int i = 0; i < job_queue_count - 1; i++) job_queue[i] = job_queue[i + 1];
    job_queue_count--;
    return p;
}

void enqueue_ready(Process* p) {
    if (ready_queue_count < MAX_PROCESSES_IN_MEMORY) {
        ready_queue[ready_queue_tail] = p;
        ready_queue_tail = (ready_queue_tail + 1) % (MAX_PROCESSES_IN_MEMORY + 1);
        ready_queue_count++;
        p->state = READY;
    } else printf("  [进程调度]: 就绪队列已满。\n");
}

Process* dequeue_ready() {
    if (ready_queue_count == 0) return NULL;
    Process* p = ready_queue[ready_queue_head];
    ready_queue_head = (ready_queue_head + 1) % (MAX_PROCESSES_IN_MEMORY + 1);
    ready_queue_count--;
    return p;
}

void enqueue_blocked(Process* p) {
    if (blocked_queue_count < MAX_PROCESSES_IN_MEMORY) {
        blocked_queue[blocked_queue_count++] = p;
        p->state = BLOCKED;
        p->block_start_time = current_time;
        p->block_duration = 2 + (rand() % 4); // 随机阻塞2-5个时间单位
        printf("  [进程调度]: 进程 %d 随机阻塞 %d 个时间单位。\n", p->pid, p->block_duration);
    } else printf("  [进程调度]: 阻塞队列已满。\n");
}

void remove_from_blocked_queue(int pid) {
    for (int i = 0; i < blocked_queue_count; i++) {
        if (blocked_queue[i]->pid == pid) {
            for (int j = i; j < blocked_queue_count - 1; j++) blocked_queue[j] = blocked_queue[j + 1];
            blocked_queue_count--;
            return;
        }
    }
}

// --- 主要逻辑 ---
void generate_random_job() {
    if (job_queue_count < MAX_JOBS) {
        Process* new_job = (Process*)malloc(sizeof(Process));
        new_job->job_id = next_job_id++;
        new_job->pid = -1;
        new_job->state = JOB_READY;
        new_job->required_cpu_time = 5 + (rand() % 10); // CPU时间5-14
        new_job->current_cpu_time = 0;
        new_job->memory_size = 50 + (rand() % 200); // 内存50-249K
        new_job->memory_start = -1;
        new_job->time_slice_used = 0;
        new_job->block_start_time = 0;
        new_job->block_duration = 0;
        enqueue_job(new_job);
        printf("  [作业生成]: 作业 %d 进入后备队列。\n", new_job->job_id);
    } else printf("  [作业生成]: 后备队列已满。\n");
}

// 将作业加载到内存并创建进程
void load_jobs_to_memory() {
    while (job_queue_count > 0 && (ready_queue_count + (running_process != NULL ? 1 : 0) + blocked_queue_count) < MAX_PROCESSES_IN_MEMORY) {
        Process* job_to_load = dequeue_job();
        if (job_to_load == NULL) break;
        
        int mem_start = allocate_memory(next_pid, job_to_load->memory_size);
        if (mem_start != -1) { // 内存分配成功
            job_to_load->pid = next_pid++;
            job_to_load->memory_start = mem_start;
            enqueue_ready(job_to_load);
            printf("  [内存管理]: 作业 %d (PID: %d) 进入内存，分配 %dK @ %d。\n",
                   job_to_load->job_id, job_to_load->pid, job_to_load->memory_size, job_to_load->memory_start);
        } else { // 内存不足
            printf("  [内存管理]: 内存不足，作业 %d 无法进入内存。\n", job_to_load->job_id);
            free(job_to_load); // 简化处理，直接丢弃
            break;
        }
    }
}

// 检查并唤醒阻塞进程
void check_blocked_processes() {
    for (int i = blocked_queue_count - 1; i >= 0; i--) {
        Process* p = blocked_queue[i];
        if (current_time >= (p->block_start_time + p->block_duration)) {
            remove_from_blocked_queue(p->pid);
            enqueue_ready(p);
            printf("  [进程调度]: 进程 %d 阻塞结束，进入就绪队列。\n", p->pid);
        }
    }
}

// 进程调度器 (时间片轮转)
void scheduler() {
    if (running_process != NULL) {
        running_process->current_cpu_time++;
        running_process->time_slice_used++;
        printf("  [调度器]: 进程 %d 运行中 (CPU: %d/%d, 片已用: %d/%d)。\n",
               running_process->pid, running_process->current_cpu_time,
               running_process->required_cpu_time, running_process->time_slice_used, TIME_QUANTUM);

        if (rand() % 10 == 0) { // 约10%概率阻塞
            printf("  [调度器]: 进程 %d 随机触发阻塞。\n", running_process->pid);
            enqueue_blocked(running_process);
            running_process = NULL; return;
        }

        if (running_process->current_cpu_time >= running_process->required_cpu_time) { // 进程完成
            printf("  [调度器]: 进程 %d (作业 %d) 完成。\n", running_process->pid, running_process->job_id);
            running_process->state = TERMINATED;
            completed_jobs[completed_jobs_count++] = running_process;
            free_memory(running_process->pid);
            running_process = NULL;
        } else if (running_process->time_slice_used >= TIME_QUANTUM) { // 时间片用完
            printf("  [调度器]: 进程 %d 时间片用完，被抢占。\n", running_process->pid);
            running_process->time_slice_used = 0;
            enqueue_ready(running_process);
            running_process = NULL;
        }
    }

    if (running_process == NULL && ready_queue_count > 0) { // 选择下一个运行进程
        running_process = dequeue_ready();
        if (running_process != NULL) {
            running_process->state = RUNNING;
            running_process->time_slice_used = 0;
            printf("  [调度器]: 进程 %d 开始运行。\n", running_process->pid);
        }
    } else if (running_process == NULL && ready_queue_count == 0) {
        printf("  [调度器]: CPU 空闲。\n");
    }
}

// --- 显示函数 ---
void print_memory_map() {
    printf("--- 内存分配情况 (0-%dK) ---\n", USER_MEMORY_SIZE);
    printf("范围         大小(K) 状态 拥有者PID\n");
    printf("-----------------------------------\n");
    for (int i = 0; i < num_memory_blocks; i++) {
        printf("%04dK-%04dK %-8d %-4s %-8d\n",
               memory_map[i].start_address,
               memory_map[i].start_address + memory_map[i].size - 1,
               memory_map[i].size,
               memory_map[i].is_free ? "空闲" : "占用",
               memory_map[i].owner_pid);
    }
    printf("-----------------------------------\n");
}

void print_process_info(Process* p) {
    if (p == NULL) { printf("  (空)\n"); return; }
    printf("  PID: %d (作业 %d) | 状态: %s | CPU: %d/%d | 内存: %dK@%d\n",
           p->pid, p->job_id,
           p->state == JOB_READY ? "作业就绪" : (p->state == READY ? "就绪" :
           (p->state == RUNNING ? "运行" : (p->state == BLOCKED ? "阻塞" : "完成"))),
           p->current_cpu_time, p->required_cpu_time, p->memory_size, p->memory_start);
}

void print_queue_info(const char* name, Process** queue, int count, int is_circular) {
    printf("--- %s (%d个) ---\n", name, count);
    if (count == 0) { printf("  (队列为空)\n"); return; }
    if (is_circular) {
        for (int i = 0; i < count; i++) print_process_info(queue[(ready_queue_head + i) % (MAX_PROCESSES_IN_MEMORY + 1)]);
    } else {
        for (int i = 0; i < count; i++) print_process_info(queue[i]);
    }
}

void display_status() {
    printf("\n====================================\n");
    printf("模拟时间: %ld\n", current_time);
    printf("====================================\n");
    print_queue_info("后备作业队列", job_queue, job_queue_count, 0);
    print_memory_map();
    printf("--- 进程信息 ---\n");
    printf("运行中进程: "); print_process_info(running_process);
    print_queue_info("就绪队列", ready_queue, ready_queue_count, 1);
    print_queue_info("阻塞队列", blocked_queue, blocked_queue_count, 0);
    printf("--- 完成作业情况 (%d个) ---\n", completed_jobs_count);
    if (completed_jobs_count == 0) { printf("  (暂无完成作业)\n"); }
    else { for (int i = 0; i < completed_jobs_count; i++) printf("  作业 %d (PID %d) 完成，CPU: %d。\n", completed_jobs[i]->job_id, completed_jobs[i]->pid, completed_jobs[i]->current_cpu_time); }
    printf("====================================\n");
}

// --- 主函数 ---
int main() {
    init_os();
    for (int i = 0; i < 10; i++) generate_random_job(); // 初始提交10个作业

    for (int tick = 0; tick < 50; tick++) { // 模拟50个时间单位
        current_time++;
        printf("\n--- 时间片 %ld ---\n", current_time);
        if (current_time % 5 == 0 && job_queue_count < MAX_JOBS) generate_random_job(); // 定期生成新作业
        
        load_jobs_to_memory();
        check_blocked_processes();
        scheduler();
        display_status();

        if (job_queue_count == 0 && ready_queue_count == 0 && blocked_queue_count == 0 && running_process == NULL && completed_jobs_count == next_job_id) {
            printf("\n所有作业已完成，模拟器停止。\n");
            break;
        }
        // sleep(1); // Unix/Linux 下用 sleep (秒), 网页环境不使用
    }

    printf("\n--- 模拟结束 ---\n");
    // 释放内存
    for (int i = 0; i < completed_jobs_count; i++) free(completed_jobs[i]);
    for (int i = 0; i < job_queue_count; i++) free(job_queue[i]);
    while (ready_queue_count > 0) free(dequeue_ready());
    for (int i = 0; i < blocked_queue_count; i++) free(blocked_queue[i]);
    if (running_process != NULL) free(running_process);
    
    return 0;
}
