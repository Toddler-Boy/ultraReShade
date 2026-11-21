// reshade_ultra_api.h: the one form of the C APIs between ultraGame's binaries (the manager, its engine modules, the image add-on) and
// between ultraReShade and the add-on that draws its overlay. Each side asks the other for its functions by name; nothing here names a
// capability, names are text where they are asked and answered.
// The rules, binding on both sides of every call:
// - Every name and every callback handed over in a cell is a UgCall: the caller's cells (a record) and their count, the context a
//   callback was handed with (null for a name). Inputs and results are cells of that record; no name has a signature of its own.
// - The side that implements a name or callback lays out its record (cell 1 on) and answers "record.start" and "record.slot" for it,
//   under the name, or for a callback under the name its cell gives ("game.start", "panel.get"). The caller asks the slots once, when
//   it finds the other side (again after that side reloaded), and from then on writes and reads plain cells: nothing by string per call.
// - A call's cells are record.start's count of them, its defaults in, cell 0 cleared: a value for a name the callee does not know
//   lands in cell 0 and is lost, a name the caller does not know reads the callee's default.
// - The callee refuses (0, one log line) a call with fewer cells than its record has or without a value it needs; it reads and writes
//   only within count, through a pointer cell only as far as the size or count cell beside it allows. Numbers out of range are clamped
//   or refused. A text is copied by the side that keeps it.
// - A name the other side lacks: one log line, that capability off. "dev." marks exactly Developer mode's names and cells: a
//   a side with it off neither asks for nor answers them.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 8 bytes on 32 and 64 bits: a pointer or 32-bit value goes into a cleared cell
typedef union UgCell
{
	uint64_t	u64;
	int64_t		i64;
	double		f64;
	float		f32;
	uint32_t	u32;
	int32_t		i32;
	const void*	ptr;
	const char*	text;		// UTF-8
} UgCell;

// A cell's type, as "record.slot" is asked
enum { ugU32 = 1, ugI32, ugF32, ugF64, ugU64, ugPtr, ugText, ugI64 };

// 1: done, the results in the cells; 0: not done (refused, logged once by the callee, or what it does failed)
typedef int ( *UgCall ) ( void* context, UgCell* cells, uint32_t count );

// A function of the other side by its name, null for a name it does not have. Every name is a UgCall except the two record
// functions, "record.start" (a UgRecordStart) and "record.slot" (a UgRecordSlot)
typedef void* ( *UgLookup ) ( const char* name );

// "record.start": the record's cells with their defaults, as many as count holds, cell 0 cleared; its cell count (cells null: the
// count alone, 0 for a record it does not have). "record.slot": the cell of that name and type, 0 for a name it does not have or one
// of another type (logged once)
typedef uint32_t ( *UgRecordStart ) ( const char* record, UgCell* cells, uint32_t count );
typedef uint32_t ( *UgRecordSlot ) ( const char* record, const char* name, uint32_t type );

// A binary's one export, called right after its load: the loader's lookup in, its own out
#define UG_ENGINE_EXPORT	"ultraGameEngine"
typedef UgLookup ( *UgExport ) ( UgLookup other );

#ifdef __cplusplus
}
#endif
