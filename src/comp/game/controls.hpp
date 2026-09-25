#pragma once

namespace comp::game
{
	// Moves steering, accelerate and brake onto the arrow keys, and the external camera the
	// arrows normally drive onto numpad 8/2/4/6, in the loaded key map only: the key-map
	// files on disk keep the player's own bindings.
	void install_arrow_key_driving();
}
