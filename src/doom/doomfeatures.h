//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     List of features which can be enabled/disabled to slim down the
//     program.
//

#ifndef DOOM_FEATURES_H
#define DOOM_FEATURES_H

// Enables wad merging (the '-merge' command line parameter)

#undef FEATURE_WAD_MERGE

// Enables dehacked support ('-deh')

#undef FEATURE_DEHACKED

// Enables multiplayer support (network games)

#undef FEATURE_MULTIPLAYER

// Enables sound output
//
// ExoDoom (SCRUM-101): on, backed by the PC speaker. DG_sound_module and
// DG_music_module are defined in src/doom_sound.c, outside this tree.
// Defined here rather than with -DFEATURE_SOUND: i_sound.c's
// `#include <SDL_mixer.h>` guard sits above its include of this header, so
// it never sees this definition, while sound_modules[] and
// InitMusicModule() below that include do.

#define FEATURE_SOUND

#endif /* #ifndef DOOM_FEATURES_H */


