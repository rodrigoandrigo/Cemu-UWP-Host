#pragma once

#include <wrl.h>

namespace DX
{
	// Classe do auxiliar para tempo de animação e de simulação.
	class StepTimer
	{
	public:
		StepTimer() : 
			m_elapsedTicks(0),
			m_totalTicks(0),
			m_leftOverTicks(0),
			m_frameCount(0),
			m_framesPerSecond(0),
			m_framesThisSecond(0),
			m_qpcSecondCounter(0),
			m_isFixedTimeStep(false),
			m_targetElapsedTicks(TicksPerSecond / 60)
		{
			if (!QueryPerformanceFrequency(&m_qpcFrequency))
			{
				throw ref new Platform::FailureException();
			}

			if (!QueryPerformanceCounter(&m_qpcLastTime))
			{
				throw ref new Platform::FailureException();
			}

			// Inicialize delta máxima para 1/10 de segundo.
			m_qpcMaxDelta = m_qpcFrequency.QuadPart / 10;
		}

		// Obtenha o tempo decorrido desde a chamada de Atualização anterior.
		uint64 GetElapsedTicks() const						{ return m_elapsedTicks; }
		double GetElapsedSeconds() const					{ return TicksToSeconds(m_elapsedTicks); }

		// Obtenha o tempo total desde o início do programa.
		uint64 GetTotalTicks() const						{ return m_totalTicks; }
		double GetTotalSeconds() const						{ return TicksToSeconds(m_totalTicks); }

		// Obtenha o número total de atualizações desde o início do programa.
		uint32 GetFrameCount() const						{ return m_frameCount; }

		// Obtenha a taxa de quadros atual.
		uint32 GetFramesPerSecond() const					{ return m_framesPerSecond; }

		// Defina se deseja usar o modo TimeStep fixo ou variável.
		void SetFixedTimeStep(bool isFixedTimestep)			{ m_isFixedTimeStep = isFixedTimestep; }

		// Defina a frequência para chamar a Atualização quando estiver no modo TimeStep fixo.
		void SetTargetElapsedTicks(uint64 targetElapsed)	{ m_targetElapsedTicks = targetElapsed; }
		void SetTargetElapsedSeconds(double targetElapsed)	{ m_targetElapsedTicks = SecondsToTicks(targetElapsed); }

		// O formato inteiro representa o tempo usando 10.000.000 tiques por segundo.
		static const uint64 TicksPerSecond = 10000000;

		static double TicksToSeconds(uint64 ticks)			{ return static_cast<double>(ticks) / TicksPerSecond; }
		static uint64 SecondsToTicks(double seconds)		{ return static_cast<uint64>(seconds * TicksPerSecond); }

		// Depois de uma descontinuidade de tempo intencional (por exemplo, uma operação de bloqueio de E/S)
		// chame isso para evitar que a lógica do TimeStep fixo tente realizar um conjunto de atualização 
		// Chamadas de atualização.

		void ResetElapsedTime()
		{
			if (!QueryPerformanceCounter(&m_qpcLastTime))
			{
				throw ref new Platform::FailureException();
			}

			m_leftOverTicks = 0;
			m_framesPerSecond = 0;
			m_framesThisSecond = 0;
			m_qpcSecondCounter = 0;
		}

		// Atualize o estado do temporizador, chamando a função Atualizar especificada o número determinado de vezes.
		template<typename TUpdate>
		void Tick(const TUpdate& update)
		{
			// Consulte o tempo atual.
			LARGE_INTEGER currentTime;

			if (!QueryPerformanceCounter(&currentTime))
			{
				throw ref new Platform::FailureException();
			}

			uint64 timeDelta = currentTime.QuadPart - m_qpcLastTime.QuadPart;

			m_qpcLastTime = currentTime;
			m_qpcSecondCounter += timeDelta;

			// Fixe deltas de tempo excessivamente longos (por exemplo: após pausa no depurador).
			if (timeDelta > m_qpcMaxDelta)
			{
				timeDelta = m_qpcMaxDelta;
			}

			// Converta unidades QPC em um formato de tique canônico. Não será possível estourar devido à fixação anterior.
			timeDelta *= TicksPerSecond;
			timeDelta /= m_qpcFrequency.QuadPart;

			uint32 lastFrameCount = m_frameCount;

			if (m_isFixedTimeStep)
			{
				// Lógica de atualização de TimeStep fixo

				// Se o aplicativo estiver sendo executado muito próximo do tempo decorrido definido (dentro de 1/4 de milissegundo), basta restringir
				// o relógio para corresponder exatamente ao valor de destino. Isso evita que erros pequenos e irrelevantes
				// acumulem com o passar do tempo. Sem essa fixação, um jogo que solicitava 60 fps
				// de atualização fixa, executando com vsync habilitado em uma tela NTSC de 59,94, acumularia
				// com o tempo pequenos erros suficientes para remover um quadro. O melhor é apenas arredondar 
				// pequenos desvios para zero para que tudo funcione perfeitamente.

				if (abs(static_cast<int64>(timeDelta - m_targetElapsedTicks)) < TicksPerSecond / 4000)
				{
					timeDelta = m_targetElapsedTicks;
				}

				m_leftOverTicks += timeDelta;

				while (m_leftOverTicks >= m_targetElapsedTicks)
				{
					m_elapsedTicks = m_targetElapsedTicks;
					m_totalTicks += m_targetElapsedTicks;
					m_leftOverTicks -= m_targetElapsedTicks;
					m_frameCount++;

					update();
				}
			}
			else
			{
				// Lógica de atualização de TimeStep variável.
				m_elapsedTicks = timeDelta;
				m_totalTicks += timeDelta;
				m_leftOverTicks = 0;
				m_frameCount++;

				update();
			}

			// Monitore a taxa de quadros atual.
			if (m_frameCount != lastFrameCount)
			{
				m_framesThisSecond++;
			}

			if (m_qpcSecondCounter >= static_cast<uint64>(m_qpcFrequency.QuadPart))
			{
				m_framesPerSecond = m_framesThisSecond;
				m_framesThisSecond = 0;
				m_qpcSecondCounter %= m_qpcFrequency.QuadPart;
			}
		}

	private:
		// Os dados de tempo de origem usam unidades QPC.
		LARGE_INTEGER m_qpcFrequency;
		LARGE_INTEGER m_qpcLastTime;
		uint64 m_qpcMaxDelta;

		// Os dados de tempo derivados usam um formato de tique canônico.
		uint64 m_elapsedTicks;
		uint64 m_totalTicks;
		uint64 m_leftOverTicks;

		// Membros para acompanhar a taxa de quadros.
		uint32 m_frameCount;
		uint32 m_framesPerSecond;
		uint32 m_framesThisSecond;
		uint64 m_qpcSecondCounter;

		// Membros para configurar o modo Timestep fixo.
		bool m_isFixedTimeStep;
		uint64 m_targetElapsedTicks;
	};
}
