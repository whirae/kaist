#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

// 유효한 스레드를 구분하기 위해 사용되는 상수.
// 스레드 구조체의 멤버에 MAGIC 값이 저장되고, 이르 통해 유효성 검사가 이루어짐.
// 스레드가 유효하지 않은 경우, MAGIC 값이 일치하지 않게 되어 오류 감지할 수 있다
#define THREAD_MAGIC 0xcd6abf4b

// 기본 스레드에 사용되는 임의의 값.
// 이 값은 스레드 관리의 핵심적인 부분이 아니므로 변경하지 말 것
#define THREAD_BASIC 0xd42df210

// ready_list는 실행 준비가 완료된 스레드들을 관리하는 리스트.
// 대기 중인 스레드들이 이 리스트에 추가되어 스케줄러에 의해 선택된다
static struct list ready_list;

// idle 스레드는 CPU가 아무 작업도 하지 않을 때 실행되는 스레드.
// 유휴 상태일 때 CPU는 이 스레드를 실행
static struct thread *idle_thread;

// 초기 스레드를 나타낸다. Pintos가 시작될 때 최초로 생성되는 스레드
static struct thread *initial_thread;

// 스레드 고유 ID를 생성하기 위한 lock 변수
// 멀티스레드 환경에서 고유 ID를 안전하게 생성하기 위해 사용
static struct lock tid_lock;

// 스레드 제거 요청을 담고 있는 리스트
// 삭제 요청이 들어온 스레드들이 이 리스트에 추가된다
static struct list destruction_req;

// 스레드가 실행 중일 때 통계를 추적하는 변수들
// idle_ticks는 CPU가 유휴 상태일 때 증가하고,
// kernel_ticks는 커널 모드에서 스레드가 실행 중일 때 증가하며,
// user_ticks는 사용자 모드에서 스레드가 실행 중일 때 증가한다.
static long long idle_ticks;    
static long long kernel_ticks;  
static long long user_ticks;   

// 타임 슬라이스 : 스레드가 CPU를 점유할 수 있는 시간 단위.
// 'thread_ticks'는 현재 실행 중인 스레드의 타이머 틱을 추적한다.
#define TIME_SLICE 4            // 각 스레드에 주어지는 타이머 틱 수
static unsigned thread_ticks;   // 현재 스레드의 타임 슬라이스를 추적

// 스케줄러가 rr방식 대신 다중 레벨 피드백 큐(multi-level Feedback Queue)를 사용할지 
// 여부를 나타내는 플래그. 기본값 false이며, 커널 명령줄 옵션 '-o mlfqs'를 통해 설정
bool thread_mlfqs;

// 커널 스레드 함수 포인터 선언
// 스레드가 실행항 함수와 그에 전달될 인자를 지정
static void kernel_thread (thread_func *, void *aux);

// idle 스레드를 초기화하는 함수.
// 이 함수는 CPU가 유휴 상태일 때 실행
static void idle (void *aux UNUSED);
// 다음 실행할 스레드를 반환하는 함수.
// 스케줄러가 실행 준비가 된 스레드 중에서 선택하여 반환한다.
static struct thread *next_thread_to_run (void);
// 새로운 스레드를 초기화하는 함수
// 스레드의 이름, 우선순위 등을 설정
static void init_thread (struct thread *, const char *name, int priority);
// 현재 스레드의 상태를 변경하는 함수
// 스레드가 종료되거나 차단되었을 때 상태를 업데이트
static void do_schedule(int status);
// 스케줄링을 수행하는 함수.
// 현재 스레드를 멈추고 다른 스레드로 교체한다.
static void schedule (void);
// 고유한 스레드 ID를 생성하는 함수.
// 새로운 스레드가 생성될 때 고유한 ID를 할당하기 위해 사용된다.
static tid_t allocate_tid (void);

// 유효한 스레드인지 검사하는 매크로.
// 스레드 포인터가 NULL이 아니고 MAGIC 값이 일치하는지 확인하여 유효성 검사를 수행한다.
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

// 현재 실행 중인 스레드를 반환하는 매크로.
// CPU의 스택 포인터를 읽어서 해당 스레드가 위치한 페이지의 시작 부분을 찾아 스레드를 식별한다.
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// GDT는 커널과 사용자 모드 간의 전환을 관리하며,
// 스레드 시작을 위한 임시 GDT를 설정함
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 스레딩 시스템을 초기화하는 함수.
   현재 실행 중인 코드를 스레드로 변환한다. 
   이는 일반적으로는 불가능하지만, loader.S가 
	 스택의 바닥을 페이지 경계에 맞추어 실행하기 때문에 가능하다.

   또한 run queue(스케줄러 큐)와 tid_lock을 초기화한다.

   이 함수를 호출한 후에는 페이지 할당자를 초기화하여,
   이후에 thread_create() 함수를 호출할 수 있도록 준비해야 한다.

   thread_init() 함수가 끝날 때까지는 thread_current()를 호출하는 것이 안전하지 않다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);	// 인터럽트가 비황성화된 상태에서만 실행 가능

	/* 임시 GDT(Global Descriptor Table)를 로드한다.
	   이 GDT는 사용자 컨텍스트를 포함하지 않으며,
	   이후 커널이 사용자 컨텍스트를 포함하는 GDT를 다시 빌드할 것이다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	// 전역 스레드 컨텍스트 초기화
	lock_init (&tid_lock);		// 고유 ID 생성에 필요한 락 초기화
	list_init (&ready_list);	// 준비 리스트 초기화
	list_init (&destruction_req);	// 스레드 제거 요청 리스트 초기화

	// 현재 실행 중인 스레드의 구조체 설정
	initial_thread = running_thread ();	// 현재 실행 중인 스레드를 초기화
	init_thread (initial_thread, "main", PRI_DEFAULT);	// 초기 스레드 설정
	initial_thread->status = THREAD_RUNNING;	// 초기화 스레드는 실행 중 상태로 설정
	initial_thread->tid = allocate_tid ();		// 고유 스레드 ID 할당
}

// 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작하는 함수.
// 또한 idle 스레드를 생성한다.
void
thread_start (void) {
	// idle 스레드를 생성한다
	struct semaphore idle_started;
	sema_init (&idle_started, 0);	// idle 스레드가 시작될 때까지 기다리기 위해 세마포어 초기화
	thread_create ("idle", PRI_MIN, idle, &idle_started);	// idle 스레드 생성

	// 인터럽트를 활성화하여 스레드 스케줄링을 시작한다
	intr_enable ();

	// idle 스레드가 초기화될 때까지 대기한다.
	sema_down (&idle_started);	// idle 스레드가 실행되면 세마포어가 해제됨
}

// 타이머 인터럽트 핸들러에 의해 각 타이머 틱마다 호출되는 함수.
// 따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행된다.
void
thread_tick (void) {
	struct thread *t = thread_current ();	// 현재 실행 중인 스레드를 가져옴

	// 스레드 통계를 업데이트
	if (t == idle_thread)	// 현재 스레드가 idle 스레드인 경우
		idle_ticks++;				// idle 상태에서 틱을 증가
#ifdef USERPROG
	else if (t->pml4 != NULL)	// 사용자 프로그램을 실행 중인 경우
		user_ticks++;						// 사용자 틱을 증가
#endif
	else
		kernel_ticks++;					// 커널 모드에서 실행 중일 때 커널 틱을 증가

	// 타임 슬라이스가 다 된 경우 선점 스케줄링을 실행한다.
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}


// 스레드 통계를 출력하는 함수.
// idle_ticks, kernel_ticks, user_ticks를 출력하여 CPU의 사용 통계를 보여준다
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 새로운 커널 스레드를 생성하는 함수.
   NAME이라는 이름과 PRIORITY를 갖는 새로운 스레드를 생성하고 ready queue에 추가한다.
   스레드가 실행할 FUNCTION과 AUX 인자를 전달한다. 
   성공 시 스레드 ID를 반환하며, 실패 시 TID_ERROR를 반환한다.

   이 함수는 thread_start()가 호출된 후에도 스레드가 생성될 수 있으며,
   새 스레드는 thread_create()가 반환되기 전에 스케줄링될 수 있다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);	// function 인자가 NULL이면 오류 발생

	// 스레드 메모리 할당
	t = palloc_get_page (PAL_ZERO);	// 페이지를 할당받아 스레드 구조체 메모리 초기화
	if (t == NULL)		// 할당 실패 시 TID_ERROR 반환
		return TID_ERROR;

	// 스레드 초기화
	init_thread (t, name, priority);	// 새 스레드 구조체 초기화
	tid = t->tid = allocate_tid ();		// 스레드 ID 할당

	// 새 스레드를 실행하기 위해 커널 스레드로 설정
	// rdi는 첫 번째 인자, rsi는 두 번째 인자로 설정된다
	t->tf.rip = (uintptr_t) kernel_thread;	// 스레드가 실행할 함수 설정
	t->tf.R.rdi = (uint64_t) function;			// 첫 번째 인자로 function 설정
	t->tf.R.rsi = (uint64_t) aux;						// 두 번째 인자로 aux 설정
	t->tf.ds = SEL_KDSEG;										// 데이터 세그먼트 셀렉터
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;										// 코드 세그먼트 셀렉터
	t->tf.eflags = FLAG_IF;									// 인터럽트 활성화

	// 스레드를 실행 대기 큐에 추가
	thread_unblock (t);

	return tid;
}

/* 현재 실행 중인 스레드를 차단(block)하는 함수.
   스레드는 thread_unblock()을 통해 깨워질 때까지 스케줄링되지 않는다.
   이 함수는 반드시 인터럽트가 비활성화된 상태에서 호출해야 한다. */
void
thread_block (void) {
	ASSERT (!intr_context ());		// 인터럽트 컨텍스트에서 호출되면 안 된다.
	ASSERT (intr_get_level () == INTR_OFF);	// 인터럽트가 비활성화된 상태여야 한다.
	thread_current ()->status = THREAD_BLOCKED;	// 현재 스레드를 BLOCKED 상태로 변경
	schedule ();	// 스케줄링 수행
}

/* 차단된 스레드 T를 실행 대기 상태로 전환하는 함수.
   T가 차단 상태가 아니라면 오류이다. 
	 (실행 중인 스레드를 대기 상태로 만들려면 thread_yield() 사용)
   이 함수는 현재 실행 중인 스레드를 선점하지 않는다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));	// T가 유효한 스레드인지 검사

	old_level = intr_disable ();		// 인터럽트를 비활성화하여 원자적 작업을 보장
	ASSERT (t->status == THREAD_BLOCKED);	// T가 차단 상태인지 확인
	list_push_back (&ready_list, &t->elem);	// ready_list에 스레드를 추가
	t->status = THREAD_READY;	// 스레드를 준비 상태로 변경
	intr_set_level (old_level);	// 이전 인터럽트 레벨 복원
}

// 현재 실행 중인 스레드의 이름을 반환하는 함수
const char *
thread_name (void) {
	return thread_current ()->name;	// 현재 스레드의 이름 반환
}

// 현재 실행 중인 스레드를 반환하는 함수
// 이는 running_thread()와 동일하지만, 몇 가지 안전성 검사를 추가한다.
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();	// 현재 실행 중인 스레드 포인터 반환

	// 스레드 T가 유효한지 확인
	// 만약 assertion이 실패하면 스택 오버플로우 문제가 발생했을 수 있다
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);	// 스레드가 실행 중 상태인지 확인

	return t;
}

// 현재 스레드의 TID를 반환하는 함수
tid_t
thread_tid (void) {
	return thread_current ()->tid;	// 현재 스레드의 TID 반환
}

// 현재 스레드를 스케줄에서 제거하고 종료시키는 함수
// 호출자에게는 절대 반환되지 않는다
void
thread_exit (void) {
	ASSERT (!intr_context ());	// 인터럽트 컨텍스트에서 호출되면 안 된다

#ifdef USERPROG
	process_exit ();	// 사용자 프로그램이 있는 경우 프로세스 종료
#endif

	// 현재 스레드의 상태를 종료로 설정하고 다른 프로세스를 스케줄한다.
	// 스레드는 schedule_tail() 호출 시 제거된다.
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();	// 이 위치에 도달할 수 없음
}

// 현재 스레드가 CPU를 양보하는 함수
// 현재 스레드는 잠들지 않고, 스케줄러에 의해 다시 선택될 수 있다
void
thread_yield (void) {
	struct thread *curr = thread_current ();	// 현재 실행 중인 스레드
	enum intr_level old_level;

	ASSERT (!intr_context ());	//인터럽트 컨텍스트에 있지 않은지 확인
	// 인터럽트 컨텍스트 : 인터럽트 핸들러를실행하고 있는 상태

	old_level = intr_disable ();	// 인터럽트 비활성화
	if (curr != idle_thread)			// 현재 스레드가 idle 스레드가 아닌 경우
		list_push_back (&ready_list, &curr->elem);	// 스레드를 ready_list에 추가
	do_schedule (THREAD_READY);			// 스케줄러 호출하여 다른 스레드로 교체
	intr_set_level (old_level);			// 이전 인터럽트 레벨 복원
}

// 현재 스레드의 우선순위를 NEW_PRIORITY로 설정하는 함수
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;	// 현재 스레드의 우선순위 변경
}

// 현재 스레드의 우선순위를 반환하는 함수
int
thread_get_priority (void) {
	return thread_current ()->priority;	// 현재 스레드의 우선순위 반환
}

// 현재 스레드의 nice 값을 설정하는 함수
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

// 현재 스레드의 nice 값을 반환한는 함수
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

// 시스템의 load average 값을 100배로 반환하는 함수
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

// 현재 스레드의 recent_cpu 값을 100배로 반환하는 함수
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* idle 스레드. 다른 스레드가 실행 준비가 안 되었을 때 실행됨.

   idle 스레드는 초기에는 thread_start()에 의해 준비 목록에 추가됨.
   처음 한 번 스케줄링 되면 idle_thread를 초기화하고, 
   semaphore를 "up" 하여 thread_start()가 계속 진행할 수 있도록 하며 즉시 차단됨. 
   그 이후로 idle 스레드는 준비 목록에 나타나지 않으며,
   준비 목록이 비어 있을 때 next_thread_to_run()에서 특별한 경우로 반환됨. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();	// 현재 스레드를 idle_thread로 설정
	sema_up (idle_started);		// 초기화된 idle_thread가 실행되었다고 신호를 보냄

	for (;;) {
		// 다른 스레드가 실행되도록 함
		intr_disable ();	// 인터럽트를 비활성화하여 현재 스레드의 실행을 잠시 멈춤
		thread_block ();	// 스레드를 블록 상태로 전환

		/* 인터럽트를 재활성화하고 다음 인터럽트를 기다림.

		   `sti` 명령어는 다음 명령어의 실행이 끝날 때까지 인터럽트를 비활성화함으로써,
		   이 두 명령어는 원자적으로 실행됨. 이 원자성은 중요함;
		   그렇지 않으면, 인터럽트가 인터럽트를 재활성화하고 
			 다음 인터럽트를 기다리는 것 사이에 처리될 수 있어,
		   최대 한 클락 틱만큼의 시간을 낭비할 수 있음.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", [IA32-v3a]
		   7.11.1 "HLT Instruction"을 참조. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

// 커널 스레드의 기본으로 사용되는 함수
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);	// 전달된 함수가 NULL이 아님을 확인

	intr_enable ();      // 스케줄러가 인터럽트가 꺼진 상태에서 실행되도록 인터럽트를 활성화
	function (aux);      // 지정된 스레드 함수를 실행
	thread_exit ();      // 함수가 반환되면 스레드를 종료
}


// T를 블록된 스레드로 초기화하고 이름을 NAME으로 설정하는 기본 함수
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);	// 스래드 구조체 포인터가 NULL이 아님을 확인
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);	// 우선순위가 유효한 범위 내에 있는지 확인
	ASSERT (name != NULL);	// 스레드 이름이 NULL이 아님을 확인

	memset (t, 0, sizeof *t);		// 스레드 구조체를 0으로 초기화
	t->status = THREAD_BLOCKED;	// 스레드 상태를 블록으로 설정
	strlcpy (t->name, name, sizeof t->name);	// 스레드 이름을 복사
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);	// 스택 포인터 초기화
	t->priority = priority;		// 우선순위 설정
	t->magic = THREAD_MAGIC;	// 매직 넘버로 스레드의 무결성을 확인하기 위한 설정
}

// 다음으로 스케줄링될 스레드를 선택하고 반환한다.
// 실행 큐에서 스레드를 반환해야 하며, 실행 큐가 비어있으면 idle_thread를 반환한다.
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))		// 레디 큐가 비어있으면
		return idle_thread;						// idle_thread를 반환
	else
		// 레디 큐에서 다음 스레드를 반환
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

// iretq를 사용하여 스레드를 실행합니다
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"				// 새로운 스택 포인터 설정
			"movq 0(%%rsp),%%r15\n"		// 레지스터 복원
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"				// 스택 포인터 조정
			"movw 8(%%rsp),%%ds\n"		// 세그먼트 레지스터 복원
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"				// 스택 포인터 조정
			"iretq"										// 인터럽트 반환
			: : "g" ((uint64_t) tf) : "memory");	// tf를 매개변수로 사용
}

/* 새로운 스레드의 페이지 테이블을 활성화하여 스레드를 전환하며,
   이전 스레드가 종료되고 있다면 그것을 파괴함.

   이 함수가 호출될 때, 우리는 방금 PREV 스레드에서 전환했고,
   새로운 스레드는 이미 실행 중이며 인터럽트는 여전히 비활성화되어 있음.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않음.
   실제로, printf()는 함수 끝에 추가해야 함. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;	// 현재 스레드의 tf를 가져옴
	uint64_t tf = (uint64_t) &th->tf;					// 다음 스레드의 tf를 가져옴
	ASSERT (intr_get_level () == INTR_OFF);		// 인터럽트가 꺼져있음을 확인

	/* 주요 전환 로직.
	 * 먼저 전체 실행 컨텍스트를 intr_frame에 복원한 다음,
	 * do_iret를 호출하여 다음 스레드로 전환함.
	 * 여기서부터는 전환이 완료될 때까지 스택을 사용하면 안 됨. */
	__asm __volatile (
			// 사용할 레지스터를 저장
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			// 입력을 한 번만 가져옴
			"movq %0, %%rax\n"				// 현재 tf 주소를 rax에 로드
			"movq %1, %%rcx\n"				// 다음 tf 주소를 rcx에 로드
			"movq %%r15, 0(%%rax)\n"	// 레지스터 값을 tf에 저장
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새로운 프로세스를 스케줄링하는 함수입니다. 
   진입 시 인터럽트는 꺼져 있어야 합니다.
   현재 스레드의 상태를 지정된 상태로 변경한 후,
   실행할 다른 스레드를 찾아 전환합니다.
   schedule()에서 printf()를 호출하는 것은 안전하지 않습니다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);		// 인터럽트가 꺼져 있는지 확인
	ASSERT (thread_current()->status == THREAD_RUNNING);	// 현재 스레드의 상태가 실행 중인지 확인
	
	// 파괴 요청이 있는 스레드에 대해 메모리를 해제
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			// 파괴 요청 큐에서 스레드 가져오기
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);	// 스레드에 할당된 페이지 메모리 해제
	}

	// 현재 스레드의 상태를 변경
	thread_current ()->status = status;	// 현재 스레드의 상태르 전달받은 status로 설정
	schedule ();	// 다음 스레드로 스케줄링을 전환
}

// 현재 스레드를 다음 스레드로 전환하는 함수
static void
schedule (void) {
	struct thread *curr = running_thread ();	// 현재 실행 중인 스레드를 가져온다
	struct thread *next = next_thread_to_run ();	// 다음 실행할 스레드를 가져온다

	ASSERT (intr_get_level () == INTR_OFF);		// 인터럽트가 꺼져 있는지 확인	
	ASSERT (curr->status != THREAD_RUNNING);	// 현재 스레드의 상태가 실행 중이지 않음을 확인
	ASSERT (is_thread (next));								// 다음 스레드가 유효한 스레드인지 확인
	
	// 다음 스레드를 실행 중으로 표시
	next->status = THREAD_RUNNING;

	// 새 타임 슬라이스를 시작
	thread_ticks = 0;

#ifdef USERPROG
	// 새로운 주소 공간을 활성화한다
	process_activate (next);
#endif
	// 현재 스레드와 다음 스레드가 다를 경우
	if (curr != next) {
		// 현재 스레드가 종료 상태라면 스레드 구조체를 파괴
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);	// 현재 스레드와 다음 스레드가 같지 않음을 확인
			list_push_back (&destruction_req, &curr->elem);	// 파괴 요청 큐에 현재 스레드를 추가
		}

		// 스레드를 전환하기 전에 현재 실행 중인 스레드의 정보를 저장
		thread_launch (next);	// 다음 스레드로 전환
	}
}

// 새로운 스레드에 사용할 TID를 반환하는 함수
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;	// 다음 TID를 위한 정적 변수, 1 부터 시작
	tid_t tid;									// 할당된 TID를 저장할 변수

	lock_acquire (&tid_lock);		// TID 할당을 위한 잠금을 획득	
	tid = next_tid++;						// 현재 TID를 할당하고 증가시킴
	lock_release (&tid_lock);		// 잠금을 해제

	return tid;									// 할당된 TID를 반환
}
