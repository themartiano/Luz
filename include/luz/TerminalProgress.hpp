#pragma once

#include <chrono>
#include <string>

namespace TerminalProgress
{
	class	PhaseProgress
	{
		public:
			PhaseProgress(std::string label, bool enabled);

			void	update(unsigned int percentage);
			void	finish(double elapsedMS);

		private:
			void	print(unsigned int percentage, bool finished, double elapsedMS);
			bool	shouldPrint(unsigned int percentage, bool finished) const;

			std::string	_label;
			bool		_enabled;
			bool		_printed;
			unsigned int	_lastPercentage;
			unsigned int	_spinnerFrame;
			std::chrono::steady_clock::time_point	_lastPrintTime;
	};

	std::string	formatDuration(double milliseconds);
}
