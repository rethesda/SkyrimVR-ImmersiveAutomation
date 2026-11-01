#pragma once
#include <string>

// Immersive Winds for Fallout 4 / Fallout 4 VR: switches a fan with the simulated wind of the game.
namespace ImmersiveWindsF4
{
	const std::string MOD_VERSION = "1.0.0";

	// My Games\<Fallout4|Fallout4VR>\F4SE\immersiveWinds.ini
	std::string GetIniPath();

	// called once the game data is loaded: reads the settings and starts the threads
	void StartMod();

	// stops the plug control and switches the plug off, when the game exits
	void SwitchOffOnExit();
}
