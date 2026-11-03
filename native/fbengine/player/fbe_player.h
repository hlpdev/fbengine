#ifndef FBE_PLAYER_H
#define FBE_PLAYER_H

// Players: what plays clips on an entity and the entities under it, in layers, each over the ones
// before it. A clip's nodes are bound to entities by name as it's played, or after it changes: the
// entity itself if it has the name, else the first under it that has it, depth first, each parent
// before its children, as fbe_entity_find finds them; a node no entity has is left out, as is an
// entity freed since. A player lives while its handle is held; once its entity is freed it plays
// nothing, and its calls fail.
//
// A layer plays a clip, or more than one while a crossfade goes from one to the next, each at a
// weight going where it's going over the crossfade's seconds. Its pose is their weighted mean,
// the position and scale in a line and the rotation the short way round, over the layers before
// it by its weight times its clips' together, 1 at most; a layer with a mask moves only the
// entities it names and those under them. Each property a clip moves starts from where it was when
// a clip began moving it, so a clip faded out leaves it there; what no clip moves is left as it is.
//
// What a player poses is the game's to change until it next poses: a head turned toward a target
// after the world's update is drawn turned, and the next update poses it from its clip again. An
// entity released is the game's for good: no clip moves it until it's reclaimed, when a clip moves
// it again from where the game left it.
//
// Players are handles of their own kind; the entities and clips they're given are the scene's and
// the clip area's.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How a clip goes on at its end: holding it, from its start again, or back the way it came.
#define FBE_PLAYER_ONCE      0
#define FBE_PLAYER_LOOP      1
#define FBE_PLAYER_PING_PONG 2

// A player's layers are 0 to FBE_PLAYER_LAYERS - 1.
#define FBE_PLAYER_LAYERS 16

// A new player for `entity` and what's under it, playing nothing; 0 for an entity freed.
FBE_API uint64_t fbe_player_create(uint64_t entity);
FBE_API bool fbe_player_free(uint64_t player);
FBE_API bool fbe_player_alive(uint64_t player);
// The entity it plays on, 0 for a freed player.
FBE_API uint64_t fbe_player_entity(uint64_t player);

// Each of these takes a layer, from 0 to FBE_PLAYER_LAYERS - 1, and fails for one that isn't, or
// a player freed, or its entity.

// Plays `clip` on the layer in `mode` at `speed` times, in place of what the layer played, posing
// what it moves at once, from its start, or its end at a speed below 0. If it's what the layer's
// playing already, the one it played last and not stopping, even at its end or fading in, it only
// takes the new mode and speed, so a game can ask for it every frame; fbe_player_stop first plays
// it again. False too for a clip freed, a mode that isn't one, or a speed that isn't finite.
FBE_API bool fbe_player_play(uint64_t player, int32_t layer, uint64_t clip, int32_t mode,
                             float speed);
// Plays `clip` as fbe_player_play does, fading it in over `seconds` while what the layer played
// fades out; one being faded out goes on where it is, fading back in. With 0 seconds,
// it's fbe_player_play. False too for seconds below 0 or not finite.
FBE_API bool fbe_player_crossfade(uint64_t player, int32_t layer, uint64_t clip, float seconds,
                                  int32_t mode, float speed);
// Fades out what the layer plays over `seconds`, or with 0 stops it at once, leaving what it moved
// where it is. False too for seconds below 0 or not finite.
FBE_API bool fbe_player_stop(uint64_t player, int32_t layer, float seconds);
// The clip the layer played last, which it plays or fades in, 0 for none, as when it's stopping.
FBE_API uint64_t fbe_player_clip(uint64_t player, int32_t layer);
// Whether the layer plays its clip: false once a ONCE clip is at its end, or it's stopping.
FBE_API bool fbe_player_playing(uint64_t player, int32_t layer);

// Puts the layer's clip at `time` seconds, as its mode has it, posing what it moves at once; an
// event at that time isn't reached. False too for a time that isn't finite.
FBE_API bool fbe_player_set_time(uint64_t player, int32_t layer, float time);
// Where the layer is in its clip, in seconds, 0 for none.
FBE_API float fbe_player_time(uint64_t player, int32_t layer);
// Sets how many times as fast as made the layer plays its clip, backward below 0. False too for a
// speed that isn't finite.
FBE_API bool fbe_player_set_speed(uint64_t player, int32_t layer, float speed);
FBE_API float fbe_player_speed(uint64_t player, int32_t layer);
// Sets how much of the layer's pose is over the layers before it, from 0 to 1, as it starts. False
// too for a weight outside that.
FBE_API bool fbe_player_set_weight(uint64_t player, int32_t layer, float weight);
FBE_API float fbe_player_weight(uint64_t player, int32_t layer);
// Keeps the layer to `count` entities and those under them, or with none lets it move all it
// plays. False too for an entity freed.
FBE_API bool fbe_player_set_mask(uint64_t player, int32_t layer, const uint64_t* entities,
                                 int32_t count);

// Leaves `bone`, an entity, to the game: none of the player's clips moves it, though they go on
// moving what's under it, until it's reclaimed. Releasing one released, or reclaiming one that
// isn't, changes nothing. False for a player or an entity freed, or a player whose entity is.
FBE_API bool fbe_player_release(uint64_t player, uint64_t bone);
FBE_API bool fbe_player_reclaim(uint64_t player, uint64_t bone);
// Whether `bone` is released, false for a player or an entity freed.
FBE_API bool fbe_player_released(uint64_t player, uint64_t bone);

// Moves every player on an entity in `world` on by `seconds`, posing what each moves, as the
// world's update does, and keeps the events each reached, for fbe_player_evented. A step past many
// times round a loop reaches its events the first and the last time round only. False for a freed
// world, or seconds below 0 or not finite.
FBE_API bool fbe_player_advance(uint64_t world, float seconds);
// The players with events kept, up to `capacity` of them into `players`; gives how many there are.
FBE_API int32_t fbe_player_evented(uint64_t* players, int32_t capacity);
// How many events the player reached since they were last let go of, and the one at `event`'s
// name, its clip's name, both valid until the player next moves or they're let go of, its layer,
// and its time in its clip;
// "", "", -1, and 0 for one out of range, or a freed player.
FBE_API int32_t fbe_player_event_count(uint64_t player);
FBE_API const char* fbe_player_event_name(uint64_t player, int32_t event);
FBE_API const char* fbe_player_event_clip(uint64_t player, int32_t event);
FBE_API int32_t fbe_player_event_layer(uint64_t player, int32_t event);
FBE_API float fbe_player_event_time(uint64_t player, int32_t event);
// Lets go of the events the player reached.
FBE_API bool fbe_player_clear_events(uint64_t player);

// Takes the players an advance found their entities freed, up to `capacity` of them into `players`,
// each once, as the FB side lets go of what it keeps for them; gives how many it took.
FBE_API int32_t fbe_player_orphans(uint64_t* players, int32_t capacity);
// How many players there are, not freed.
FBE_API int64_t fbe_player_count(void);
// How many entities the player's clips name, kept as bones, 0 for a freed player.
FBE_API int32_t fbe_player_bone_count(uint64_t player);

#ifdef __cplusplus
}
#endif

#endif
