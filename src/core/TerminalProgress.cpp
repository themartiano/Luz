#include "TerminalProgress.hpp"
#include "ANSIColors.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace
{
	constexpr unsigned int	PROGRESS_UPDATE_INTERVAL_MS = 125;
	constexpr unsigned int	PHASE_LABEL_WIDTH = 14;

	std::string	padLabel(const std::string& label)
	{
		std::ostringstream stream;

		stream << std::left << std::setw(PHASE_LABEL_WIDTH) << label;
		return (stream.str());
	}
}

TerminalProgress::PhaseProgress::PhaseProgress(
	std::string label,
	bool enabled
) :
	_label(std::move(label)),
	_enabled(enabled),
	_printed(false),
	_lastPercentage(101),
	_spinnerFrame(0),
	_lastPrintTime(std::chrono::steady_clock::time_point())
{
	if (this->_enabled)
	{
		this->update(0);
	}
}

void	TerminalProgress::PhaseProgress::update(unsigned int percentage)
{
	const unsigned int clamped = std::min(percentage, 100u);

	if (!this->_enabled || !this->shouldPrint(clamped, false))
	{
		return;
	}
	this->print(clamped, false, 0.0);
}

void	TerminalProgress::PhaseProgress::finish(double elapsedMS)
{
	if (!this->_enabled)
	{
		return;
	}
	this->print(100, true, elapsedMS);
}

bool	TerminalProgress::PhaseProgress::shouldPrint(unsigned int percentage, bool finished) const
{
	if (finished || !this->_printed)
	{
		return (true);
	}
	if (percentage == this->_lastPercentage)
	{
		return (false);
	}

	const auto now = std::chrono::steady_clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		now - this->_lastPrintTime
	).count();

	return (elapsed >= PROGRESS_UPDATE_INTERVAL_MS);
}

void	TerminalProgress::PhaseProgress::print(unsigned int percentage, bool finished, double elapsedMS)
{
	const unsigned int clamped = std::min(percentage, 100u);
	static const std::array<const char*, 8> spinnerFrames = {
		"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"
	};

	std::cout
		<< "\r\033[K";
	if (!finished)
	{
		std::cout
			<< CLR_CYAN << spinnerFrames[this->_spinnerFrame % spinnerFrames.size()] << " ";
	}
	std::cout
		<< CLR_WHITE_BOLD << padLabel(this->_label) << CLR_RESET
		<< CLR_WHITE << std::setw(3) << clamped << "%" << CLR_RESET;
	if (finished)
	{
		std::cout
			<< "  " << CLR_GREEN_BRIGHT << "done"
			<< CLR_BLUE_BRIGHT << " (" << CLR_WHITE
			<< TerminalProgress::formatDuration(elapsedMS)
			<< CLR_BLUE_BRIGHT << ")" << CLR_RESET << std::endl;
	}
	else
	{
		std::cout << std::flush;
	}
	this->_printed = true;
	this->_lastPercentage = clamped;
	this->_spinnerFrame++;
	this->_lastPrintTime = std::chrono::steady_clock::now();
}

std::string	TerminalProgress::formatDuration(double milliseconds)
{
	std::ostringstream stream;
	const double safeMilliseconds = std::max(0.0, milliseconds);

	if (safeMilliseconds < 1000.0)
	{
		stream << std::fixed << std::setprecision(safeMilliseconds < 100.0 ? 2 : 1)
			<< safeMilliseconds << " ms";
		return (stream.str());
	}

	const double seconds = safeMilliseconds / 1000.0;
	if (seconds < 60.0)
	{
		stream << std::fixed << std::setprecision(seconds < 10.0 ? 2 : 1)
			<< seconds << " s";
		return (stream.str());
	}

	const int minutes = static_cast<int>(seconds / 60.0);
	const double remainingSeconds = seconds - static_cast<double>(minutes * 60);
	stream << minutes << "m " << std::fixed << std::setprecision(1) << remainingSeconds << "s";
	return (stream.str());
}
