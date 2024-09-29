#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


// 스레드의 생명 주기에서의 상태들
enum thread_status {
	THREAD_RUNNING,     // 현재 실행 중인 스레드
	THREAD_READY,       // 실행 준비, but 실행중 아님
	THREAD_BLOCKED,     // 이벤트 발생을 기다리는 상태
	THREAD_DYING        // 곧 삭제될 스레드
};

// 스레드 식별자 타입.
// 원하는 타입으로 재정의할 수 있음.
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          // tid_t의 오류 값

// 스레드 우선순위
#define PRI_MIN 0                       // 최저 우선순위
#define PRI_DEFAULT 31                  // 기본 우선순위
#define PRI_MAX 63                      // 최고 우선순위

/* 커널 스레드 또는 사용자 프로세스. 
 *
 * 각 스레드 구조체는 자신의 4KB 페이지에 저장됩니다.
 * 스레드 구조체는 페이지의 맨 아래에 위치하며(오프셋 0),
 * 나머지 페이지는 스레드의 커널 스택을 위해 예약되어 있습니다.
 * 커널 스택은 페이지의 상단에서 아래로 성장합니다.
 * 여기서는 다음과 같은 구조를 가집니다:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이 구조는 두 가지 중요한 의미가 있습니다:
 *
 *    1. `struct thread'의 크기가 너무 커져서는 안 됩니다.
 *       그렇지 않으면 커널 스택을 위한 공간이 부족해집니다.
 *       기본 `struct thread'는 몇 바이트 크기여야 하며,
 *       1KB 이하로 유지하는 것이 좋습니다.
 *
 *    2. 커널 스택이 너무 커지지 않도록 해야 합니다.
 *       스택 오버플로우가 발생하면 스레드 상태가 손상됩니다.
 *       따라서 커널 함수에서는 큰 구조체나 배열을
 *       비정적 지역 변수로 할당해서는 안 됩니다.
 *       대신 `malloc()`이나 `palloc_get_page()`와 같은
 *       동적 할당을 사용해야 합니다.
 *
 * 스택 오버플로우가 발생하면 `thread_current()`에서
 * 매직 값이 변경되어 단언문 실패가 발생할 수 있습니다. 
 */

/* `elem` 멤버는 이중 용도로 사용됩니다.
 * 실행 큐(thread.c)의 요소이거나, 
 * 세마포어 대기 목록(synch.c)의 요소일 수 있습니다.
 * 이 두 가지 방법은 상호 배타적입니다: 
 * 실행 큐에 있는 스레드는 준비 상태인 스레드만 있고,
 * 세마포어 대기 목록에는 블록 상태의 스레드만 있습니다. */
struct thread {
	// thread.c 에서 소유
	tid_t tid;                          // 스레드 식별자
	enum thread_status status;          // 스레드 상태
	char name[16];                      // 이름 (디버깅 용)
	int priority;                       // 우선순위
	int64_t wakeup_ticks;								// 스레드가 꺠워질 시각 추가

	// thread.c 와 synch.c 간 공유
	struct list_elem elem;              // 리스트 요소

#ifdef USERPROG
	/* userprog/process.c 에서 소유*/
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	// 스레드가 소유한 전체 가상 메모리 테이블
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               // 컨텍스트 스위칭 정보를 저장
	unsigned magic;                     // 스택 오버플로우 감지용
};

/* 기본값(false)일 경우 라운드 로빈 스케줄러를 사용합니다.
   true일 경우 multi-level feedback queue 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"에 의해 제어됩니다. */
extern bool thread_mlfqs;

void thread_init (void);		// 스레드 시스템 초기화
void thread_start (void);		// 스레드 시스템 시작

void thread_tick (void);					// 타이머 틱 발생시 호출
void thread_print_stats (void);		// 스레드 통계 출력

typedef void thread_func (void *aux);		// 스레드 함수 타입 정의
tid_t thread_create (const char *name, int priority, thread_func *, void *);	// 새 스레드 생성

void thread_block (void);								// 현재 스레드를 블록 상태로 변경
void thread_unblock (struct thread *);	// 주어진 스레드를 준비 상태로 변경

struct thread *thread_current (void);		// 현재 실행 중인 스레드 반환
tid_t thread_tid (void);								// 현재 스레드의 식별자 반환
const char *thread_name (void);					// 현재 스레드의 이름 반환

void thread_exit (void) NO_RETURN;			// 스레드 종료
void thread_yield (void);								// 현재 스레드를 준비 상태로 전환
void thread_sleep (int64_t ticks);			// 잠자는 스레드 리스트
void thread_wakeup (int64_t global_ticks);	// 깨울 스레드
bool cmp_thread_ticks(const struct list_elem *a, const struct list_elem *b, void *aux);

int thread_get_priority (void);					// 현재 스레드의 우선순위 반환
void thread_set_priority (int);					// 현재 스레드의 우선순위 설정

int thread_get_nice (void);							// 현재 스레드의 나이스 값 반환
void thread_set_nice (int);							// 현재 스레드의 나이스 값 설정
int thread_get_recent_cpu (void);				// 최근 CPU 사용량 반환
int thread_get_load_avg (void);					// 시스템 평균 로드 반환

void do_iret (struct intr_frame *tf);		// 인터럽트 복귀

#endif /* threads/thread.h */
