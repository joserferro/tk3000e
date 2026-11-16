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

// Funções de Temporização
//
// O ritmo da emulação vem de um temporizador periódico que libera um
// semáforo a cada período. Antes isso era feito com o IReferenceClock do
// DirectShow (dshow.h / strmiids.lib). Agora é uma thread própria baseada no
// contador de alta resolução (QueryPerformanceCounter), então o projeto não
// precisa mais do DirectShow.

#include "stdhdr.h"
#include <mmsystem.h>
#include "timer.h"
#include "tk3000e.h"
#include "janela.h"

//=============================================================================

static TEMPOPERIODICO Temporizador;
static HANDLE hSemaphore = NULL;
static bool g_bTimerActive = false;
static DWORD g_dwLastCyclePeriod = 0;

//=============================================================================
// Temporizador periódico: libera hSemaforo a cada usPeriodo microssegundos.
// Os disparos seguem um cronograma absoluto (proximo += periodo), então um
// atraso numa volta não acumula nas seguintes.

static DWORD WINAPI TempoPeriodico_Thread(LPVOID param)
{
	TEMPOPERIODICO *t = (TEMPOPERIODICO *)param;
	LARGE_INTEGER freq, agora;
	__int64 proximo;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&agora);
	proximo = agora.QuadPart + t->periodo;

	while (!t->parar)
	{
		__int64 falta;

		QueryPerformanceCounter(&agora);
		falta = proximo - agora.QuadPart;
		if (falta <= 0)
		{
			ReleaseSemaphore(t->hSemaforo, 1, NULL);	// Se já estiver no máximo (1), ignora
			proximo += t->periodo;
			if (proximo <= agora.QuadPart)				// Muito atrasado: não dispara em rajada
				proximo = agora.QuadPart + t->periodo;
		}
		else
		{
			// Dorme quase até o disparo e fecha o resto cedendo a CPU
			__int64 ms = (falta * 1000) / freq.QuadPart;
			Sleep(ms >= 2 ? (DWORD)(ms - 1) : 0);
		}
	}
	return 0;
}

//=============================================================================
int TempoPeriodico_Inicia(TEMPOPERIODICO *t, HANDLE hSemaforo, double usPeriodo)
{
	LARGE_INTEGER freq;
	DWORD id;

	if (!QueryPerformanceFrequency(&freq) || usPeriodo < 1.0)
		return 0;
	t->hSemaforo = hSemaforo;
	t->periodo   = (__int64)(usPeriodo * (double)freq.QuadPart / 1.0e6);
	if (t->periodo < 1)
		t->periodo = 1;
	t->parar     = 0;
	timeBeginPeriod(1);			// Sleep() com precisão de 1 ms
	t->hThread = CreateThread(NULL, 0, TempoPeriodico_Thread, t, 0, &id);
	if (!t->hThread)
	{
		timeEndPeriod(1);
		return 0;
	}
	SetThreadPriority(t->hThread, THREAD_PRIORITY_ABOVE_NORMAL);
	return 1;
}

//=============================================================================
void TempoPeriodico_Para(TEMPOPERIODICO *t)
{
	if (!t->hThread)
		return;
	t->parar = 1;
	WaitForSingleObject(t->hThread, 2000);
	CloseHandle(t->hThread);
	t->hThread = NULL;
	timeEndPeriod(1);
}

//=============================================================================
void Timer_InitTimer()
{
//	hSemaphore = CreateSemaphore(NULL, 0, 0x7FFFFFFF, NULL);
	hSemaphore = CreateSemaphore(NULL, 0, 1, NULL);
	if (hSemaphore == NULL)
		fprintf(stderr, "Error creating semaphore\n");
}

//=============================================================================
void Timer_UninitTimer()
{
	Timer_StopTimer();

	if (CloseHandle(hSemaphore) == 0)
	{
		FrameMostraMensagemErro("Erro ao terminar o temporizador");
	}
}

//=============================================================================
void Timer_WaitTimer()
{
	if(!g_bTimerActive)
		return;

	WaitForSingleObject(hSemaphore, INFINITE);
}

//=============================================================================
void Timer_StartTimer(DWORD dwCyclePeriod)
{
	if(g_bTimerActive && (g_dwLastCyclePeriod == dwCyclePeriod))
		return;

	Timer_StopTimer();

	double usPeriodo = (double)dwCyclePeriod / CLK_6502 * 1.e6;	// Em microssegundos

	if (!TempoPeriodico_Inicia(&Temporizador, hSemaphore, usPeriodo))
	{
		FrameMostraMensagemErro("Erro ao iniciar o temporizador");
		return;
	}

	g_dwLastCyclePeriod = dwCyclePeriod;
	g_bTimerActive = true;
}

//=============================================================================
void Timer_StopTimer()
{
	if(!g_bTimerActive)
		return;

	TempoPeriodico_Para(&Temporizador);
	g_bTimerActive = false;
}

// EOF
