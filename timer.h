/*  Emulador do computador TK3000 //e (Microdigital)
 *  por Fábio Belavenuto - Copyright (C) 2004
 *
 *  Adaptado do emulador Applewin por Michael O'Brien
 *  Part of code is Copyright (C) 2003-2004 Tom Charlesworth
 *
 *  Este arquivo é distribuido pela Licença Pública Geral GNU.
 *  Veja o arquivo Licenca.txt distribuido com este software.
 *
 *  ESTE SOFTWARE NÃO OFERECE NENHUMA GARANTIA
 *
 */

void Timer_InitTimer();
void Timer_UninitTimer();
void Timer_WaitTimer();
void Timer_StartTimer(DWORD dwCyclePeriod);
void Timer_StopTimer();

// Temporizador periódico (substitui o IReferenceClock do DirectShow)
typedef struct _TEMPOPERIODICO {
	HANDLE        hThread;
	HANDLE        hSemaforo;
	volatile LONG parar;
	__int64       periodo;		// Em ticks do QueryPerformanceCounter
} TEMPOPERIODICO;

int  TempoPeriodico_Inicia(TEMPOPERIODICO *, HANDLE, double);
void TempoPeriodico_Para(TEMPOPERIODICO *);

// EOF