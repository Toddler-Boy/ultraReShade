/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

// reshade_ultra_ui.h: ultraReShade's widgets, look, text and images for every add-on of ours that has something to show in the
// overlay. An add-on asks for the table once (include/reshade_ultra_ui.hpp wraps it) and calls through it; it keeps no widget code.
// The rules:
// - The table is one static in the DLL: its first member is its size, new members are added at the end, none is ever removed or
//   changes its signature. An add-on built older reads fewer members; one built newer than the DLL asks for more than it has and gets
//   null, so its panel stays closed (one log line on its side).
// - The calls that draw are made inside the overlay's frame, on the thread that draws it (an add-on's overlay callback). The text and
//   image calls without a frame (screenScale, placeLine, rasterizeGlyph, cachedGlyphBytes, decodeImage, the look's reads) work on any
//   thread.
// - Colours are as ImGui packs them (R in the lowest byte), sizes are unscaled pixels, to be multiplied by scale (). Texts are UTF-8,
//   ASCII in everything we write.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a widget reports in its result
#define RESHADE_UI_CHANGED		1u		// the value moved this frame
#define RESHADE_UI_COMMITTED	2u		// the edit is finished (released, clicked): time to save it

// Text faces
enum
{
	RESHADE_UI_FACE_REGULAR, RESHADE_UI_FACE_MEDIUM, RESHADE_UI_FACE_SEMIBOLD, RESHADE_UI_FACE_BOLD, RESHADE_UI_FACE_COUNT
};

// When a setting takes effect: at once, at the next start, or after something the player does
enum
{
	RESHADE_UI_APPLIES_NOW, RESHADE_UI_APPLIES_NEXT_START, RESHADE_UI_APPLIES_AFTER
};

// The colours of the look, in the order of res/panel-theme.json's colors
enum
{
	RESHADE_UI_COLOR_ACCENT, RESHADE_UI_COLOR_DANGER, RESHADE_UI_COLOR_WARNING, RESHADE_UI_COLOR_ACCENT_HOVER, RESHADE_UI_COLOR_TEXT,
	RESHADE_UI_COLOR_BRIGHT, RESHADE_UI_COLOR_MUTED, RESHADE_UI_COLOR_NEXT_START, RESHADE_UI_COLOR_APPLIES_AFTER, RESHADE_UI_COLOR_WHITE,
	RESHADE_UI_COLOR_SEPARATOR, RESHADE_UI_COLOR_PANEL, RESHADE_UI_COLOR_CONTROL, RESHADE_UI_COLOR_CONTROL_HOVER, RESHADE_UI_COLOR_TRACK,
	RESHADE_UI_COLOR_TICK, RESHADE_UI_COLOR_HANDLE, RESHADE_UI_COLOR_SWITCH_OFF, RESHADE_UI_COLOR_KNOB_OFF, RESHADE_UI_COLOR_KNOB_ON,
	RESHADE_UI_COLOR_CHEVRON, RESHADE_UI_COLOR_POPUP, RESHADE_UI_COLOR_POPUP_BORDER, RESHADE_UI_COLOR_POPUP_HOVER, RESHADE_UI_COLOR_BUTTON,
	RESHADE_UI_COLOR_BUTTON_HOVER, RESHADE_UI_COLOR_INFO_DOT, RESHADE_UI_COLOR_INFO_GLYPH, RESHADE_UI_COLOR_KEYCAP,
	RESHADE_UI_COLOR_KEYCAP_EDGE,
	RESHADE_UI_COLOR_WINDOW, RESHADE_UI_COLOR_WINDOW_EDGE, RESHADE_UI_COLOR_TAB, RESHADE_UI_COLOR_TAB_ACTIVE, RESHADE_UI_COLOR_TAB_HOVER,
	RESHADE_UI_COLOR_TAB_LINE, RESHADE_UI_COLOR_SCROLL_TRACK, RESHADE_UI_COLOR_SCROLL_THUMB, RESHADE_UI_COLOR_SCROLL_THUMB_HOVER,
	RESHADE_UI_COLOR_MESSAGE, RESHADE_UI_COLOR_MESSAGE_TEXT,
	RESHADE_UI_COLOR_COUNT
};

// The sizes of the look: first the ones res/panel-theme.json tunes (in its order), then the fixed ones
enum
{
	RESHADE_UI_SIZE_ROW_HEIGHT, RESHADE_UI_SIZE_ROW_GAP, RESHADE_UI_SIZE_COLUMN_GAP, RESHADE_UI_SIZE_INDENT, RESHADE_UI_SIZE_SEPARATOR_ABOVE,
	RESHADE_UI_SIZE_HEADING_ABOVE, RESHADE_UI_SIZE_HEADING_BELOW, RESHADE_UI_SIZE_SUBHEADING_ABOVE, RESHADE_UI_SIZE_FIRST_ROW_ABOVE,
	RESHADE_UI_SIZE_HEADING_GAP, RESHADE_UI_SIZE_SLIDER_BACK, RESHADE_UI_SIZE_SLIDER_PAD, RESHADE_UI_SIZE_VALUE_EDIT_PAD,
	RESHADE_UI_SIZE_SWITCH_WIDTH, RESHADE_UI_SIZE_SWITCH_HEIGHT, RESHADE_UI_SIZE_KNOB_SIZE, RESHADE_UI_SIZE_CHOICE_PAD,
	RESHADE_UI_SIZE_DROP_PAD_LEFT, RESHADE_UI_SIZE_DROP_PAD_RIGHT, RESHADE_UI_SIZE_MENU_PAD, RESHADE_UI_SIZE_MENU_ITEM_PAD,
	RESHADE_UI_SIZE_MENU_GAP, RESHADE_UI_SIZE_TIP_PAD_X, RESHADE_UI_SIZE_TIP_PAD_Y, RESHADE_UI_SIZE_BUTTON_PAD_X, RESHADE_UI_SIZE_BUTTON_GAP,
	RESHADE_UI_SIZE_KEY_HEIGHT, RESHADE_UI_SIZE_KEY_PAD_X, RESHADE_UI_SIZE_DISCLOSURE_SIZE, RESHADE_UI_SIZE_DISCLOSURE_RADIUS,

	RESHADE_UI_SIZE_TEXT_BODY, RESHADE_UI_SIZE_TEXT_HEADING, RESHADE_UI_SIZE_TEXT_SUBHEADING, RESHADE_UI_SIZE_TEXT_TITLE,
	RESHADE_UI_SIZE_TEXT_CHOICE, RESHADE_UI_SIZE_TEXT_VALUE, RESHADE_UI_SIZE_TEXT_KEY, RESHADE_UI_SIZE_TEXT_TIP, RESHADE_UI_SIZE_TEXT_INFO,
	RESHADE_UI_SIZE_PANEL_INSET, RESHADE_UI_SIZE_PANEL_WIDTH, RESHADE_UI_SIZE_RESET_WIDTH, RESHADE_UI_SIZE_RESET_ICON,
	RESHADE_UI_SIZE_INLINE_TOGGLE_HEIGHT, RESHADE_UI_SIZE_BIPOLAR_MIN_FILL, RESHADE_UI_SIZE_DROP_ROUNDING, RESHADE_UI_SIZE_CHEVRON_SIZE,
	RESHADE_UI_SIZE_MENU_ROUNDING, RESHADE_UI_SIZE_MENU_ITEM_ROUNDING, RESHADE_UI_SIZE_TIP_MAX_WIDTH, RESHADE_UI_SIZE_TIP_GAP,
	RESHADE_UI_SIZE_SHADOW_SPREAD, RESHADE_UI_SIZE_SHADOW_OFFSET, RESHADE_UI_SIZE_SHADOW_ALPHA, RESHADE_UI_SIZE_PICKER_SQUARE,
	RESHADE_UI_SIZE_PICKER_HUE_WIDTH, RESHADE_UI_SIZE_PICKER_GAP, RESHADE_UI_SIZE_PICKER_MARKER, RESHADE_UI_SIZE_SWATCH_EDGE,
	RESHADE_UI_SIZE_INFO_SIZE, RESHADE_UI_SIZE_INFO_GAP, RESHADE_UI_SIZE_KEY_EDGE, RESHADE_UI_SIZE_KEY_ROUNDING, RESHADE_UI_SIZE_KEY_GAP,
	RESHADE_UI_SIZE_KEYCAP_GAP, RESHADE_UI_SIZE_COVER_WIDTH, RESHADE_UI_SIZE_COVER_HEIGHT, RESHADE_UI_SIZE_COVER_ROUNDING,
	RESHADE_UI_SIZE_COVER_GAP, RESHADE_UI_SIZE_CHIP_GAP, RESHADE_UI_SIZE_CHIP_EDGE, RESHADE_UI_SIZE_DISABLED_ALPHA,
	RESHADE_UI_SIZE_SWING_SECONDS, RESHADE_UI_SIZE_SWING_STRETCH, RESHADE_UI_SIZE_FADE_SECONDS, RESHADE_UI_SIZE_TIP_DELAY,
	RESHADE_UI_SIZE_DONE_SECONDS,
	RESHADE_UI_SIZE_WINDOW_ROUNDING, RESHADE_UI_SIZE_WINDOW_PAD, RESHADE_UI_SIZE_WINDOW_MIN_WIDTH, RESHADE_UI_SIZE_WINDOW_MIN_HEIGHT,
	RESHADE_UI_SIZE_RESIZE_EDGE, RESHADE_UI_SIZE_TAB_HEIGHT, RESHADE_UI_SIZE_TAB_PAD_X, RESHADE_UI_SIZE_TAB_GAP, RESHADE_UI_SIZE_TAB_LINE,
	RESHADE_UI_SIZE_SCROLLBAR_WIDTH, RESHADE_UI_SIZE_SCROLLBAR_GAP, RESHADE_UI_SIZE_SCROLL_THUMB_MIN, RESHADE_UI_SIZE_CLOSE_SIZE,
	RESHADE_UI_SIZE_MESSAGE_PAD, RESHADE_UI_SIZE_DOT_SIZE,
	RESHADE_UI_SIZE_COUNT
};

// A slider's value as shown: value times scale, so many decimals, between prefix and suffix; sign shows + on positive values; zero,
// when set, is the text for a value that shows as 0
typedef struct ReShadeUiFormat
{
	float		scale;
	int32_t		decimals;
	const char*	suffix;
	uint32_t	sign;
	const char*	prefix;
	const char*	zero;
} ReShadeUiFormat;

// A slider's range. flags: HAS_MID marks a point with ticks, at the track's centre with MID_CENTRED (each half linear), LOG spreads
// a range over decades (lo > 0) evenly along the track; bipolar (lo < 0 < hi) fills from 0
#define RESHADE_UI_RANGE_HAS_MID		1u
#define RESHADE_UI_RANGE_MID_CENTRED	2u
#define RESHADE_UI_RANGE_LOG			4u

typedef struct ReShadeUiRange
{
	float		lo;
	float		hi;
	float		step;
	float		def;
	float		mid;
	uint32_t	flags;
} ReShadeUiRange;

// A key with its modifiers, held to exactly
#define RESHADE_UI_MOD_SHIFT	1u
#define RESHADE_UI_MOD_CTRL		2u
#define RESHADE_UI_MOD_ALT		4u

typedef struct ReShadeUiHotkey
{
	int32_t		vk;				// 0: no key
	uint32_t	modifiers;
} ReShadeUiHotkey;

// A toggle: switch and label as one click target. heading: a section's heading, which may show its hotkey; def: what Alt+click sets
typedef struct ReShadeUiToggleLook
{
	uint32_t			heading;
	ReShadeUiHotkey		hotkey;
	const char*			tooltip;
	uint32_t			applies;
	uint32_t			def;
} ReShadeUiToggleLook;

// A label in a small rounded box: outlined, or filled
typedef struct ReShadeUiChip
{
	const char*	text;
	uint32_t	filled;
	const char*	tooltip;
} ReShadeUiChip;

// Cover, title, a row of chips under it when there are any, and two status lines, a third under them when lines[ 2 ] is set; a
// texture is what image () or mask () gave
typedef struct ReShadeUiHeader
{
	const char*				title;
	const void*				titleImage;		// a mask, headerTitleHeight () tall; drawn instead of title
	const ReShadeUiChip*	chips;
	uint32_t				chipCount;
	const void*				cover;
	const char*				lines[ 3 ];
	uint32_t				lineColors[ 3 ];
} ReShadeUiHeader;

// One line of text laid out for rasterizing glyph by glyph on any thread: the glyphs are the caller's to hold (placeLine fills up to
// capacity of them and says in glyphCount how many the line has)
typedef struct ReShadeUiGlyph
{
	int32_t		index;			// the face's
	float		pen;			// from the text's start
} ReShadeUiGlyph;

typedef struct ReShadeUiPlacedLine
{
	int32_t		face;
	float		scale;
	int32_t		width;
	int32_t		height;
	int32_t		ascent;
	uint32_t	glyphCount;
} ReShadeUiPlacedLine;

// A page as the overlay draws it: sections of rows, laid out in one label and one value column by ultraReShade, whose rows say what they
// show and what they change through the callbacks below. Every callback gets the row's own context, and the frame the page is drawn with
// (the add-on's, handed to pageDraw): they are called only inside pageDraw, on the thread that draws, and a text they return stays valid
// until the next call of that callback. A callback that is null means the plain answer (always, never, none).
enum
{
	RESHADE_UI_ITEM_SLIDER, RESHADE_UI_ITEM_TOGGLE, RESHADE_UI_ITEM_OPTIONS, RESHADE_UI_ITEM_BUTTON, RESHADE_UI_ITEM_TEXT,
	RESHADE_UI_ITEM_SUBHEADING, RESHADE_UI_ITEM_FIELD, RESHADE_UI_ITEM_REPEAT, RESHADE_UI_ITEM_COLOR, RESHADE_UI_ITEM_NUMBER
};

// ROW: options as a choice row whatever their names. SET_APART: a row gap more above it, unless a heading is right above it. PRIMARY: a
// button's look. HAS_JOB: a button's action runs on a thread of the add-on's, who tells isBusy, progress and takeFinished. HAS_TONE: a text
// row's colour is tone (), else muted. NO_RESET: no reset arrow. WRAP: a text row breaks into lines to the page's width. A number is a value
// typed in, without a range, in the control column
#define RESHADE_UI_ITEM_FLAG_ROW		1u
#define RESHADE_UI_ITEM_FLAG_SET_APART	2u
#define RESHADE_UI_ITEM_FLAG_PRIMARY	4u
#define RESHADE_UI_ITEM_FLAG_HAS_JOB	8u
#define RESHADE_UI_ITEM_FLAG_HAS_TONE	16u
#define RESHADE_UI_ITEM_FLAG_NO_RESET	32u
#define RESHADE_UI_ITEM_FLAG_WRAP		64u

// Where a value lives: get and set on every change, save once the edit is finished (null: the value applies itself); isBound 0: none
typedef struct ReShadeUiBinding
{
	uint32_t	isBound;
	float		def;
	float		( *get ) ( void* context, void* frame );
	void		( *set ) ( void* context, void* frame, float value );
	void		( *save ) ( void* context, void* frame );
} ReShadeUiBinding;

typedef struct ReShadeUiItem
{
	uint32_t			kind;
	uint32_t			flags;
	const char*			label;
	const char*			tooltip;
	ReShadeUiRange		range;
	ReShadeUiFormat		format;
	const char* const*	choices;				// options: choice i stands for choiceValues[ i ], else for the value i
	uint32_t			choiceCount;
	const float*		choiceValues;
	uint32_t			applies;
	const char*			appliesWhen;
	const char*			doneText;
	uint32_t			colorDefault;
	void*				context;
	ReShadeUiBinding	value;
	ReShadeUiBinding	onOff;					// a slider's switch before its label; the slider stays live while off
	ReShadeUiHotkey		( *hotkey ) ( void* context );
	uint32_t			( *isVisible ) ( void* context, void* frame );
	const char*			( *disabledReason ) ( void* context, void* frame );		// null: enabled; "": dimmed without a reason
	const char*			( *title ) ( void* context, void* frame );				// the label as it is now
	const char*			( *line ) ( void* context, void* frame );				// a text row's; null hides it
	uint32_t			( *tone ) ( void* context, void* frame );
	float				( *top ) ( void* context );								// a slider's top, read each frame
	void				( *press ) ( void* context, void* frame );
	float				( *progress ) ( void* context );						// a running job's: 0 to 1, below 0 for none
	uint32_t			( *isBusy ) ( void* context );
	uint32_t			( *takeFinished ) ( void* context );
	const char*			( *fieldGet ) ( void* context );
	void				( *fieldSet ) ( void* context, const char* text );
	uint32_t			( *colorGet ) ( void* context );
	void				( *colorSet ) ( void* context, uint32_t color, uint32_t isDone );
	uint32_t			( *repeatCount ) ( void* context );
	const struct ReShadeUiItem*	( *repeatRows ) ( void* context, uint32_t entry, uint32_t* count );	// entry's rows, kept by the add-on
	void				( *fieldCommit ) ( void* context );						// a field's edit is finished (Enter, or another control taken)
	uint32_t			( *fieldIsSet ) ( void* context );						// a field has a value of its own: a reset arrow after it
} ReShadeUiItem;

// A section: its switch (if bound) turns all its items on and off, the rest of the heading row collapses it; either hides the sections
// that name it as their parent. lockReason holds the switch with the items live
typedef struct ReShadeUiSection
{
	const char*				label;
	const char*				tooltip;
	const char*				parent;
	void*					context;
	ReShadeUiBinding		toggle;
	ReShadeUiHotkey			( *hotkey ) ( void* context );
	uint32_t				( *isVisible ) ( void* context, void* frame );
	const char*				( *disabledReason ) ( void* context, void* frame );
	const char*				( *lockReason ) ( void* context, void* frame );
	const char*				( *title ) ( void* context, void* frame );
	const ReShadeUiItem*	items;
	uint32_t				itemCount;
	const char*				subtitle;			// in the muted colour after the title
	uint32_t				isDraggable;		// the heading moves it among the page's sections (moveSection)
} ReShadeUiSection;

// header: fills the page's header at each draw (null: no header). isCollapsed and setCollapsed keep which sections are closed;
// note, when set, hears of a section hidden or shown again by its parent
typedef struct ReShadeUiPage
{
	const char*				id;
	const ReShadeUiSection*	sections;
	uint32_t				sectionCount;
	void*					context;
	void					( *header ) ( void* context, void* frame, ReShadeUiHeader* out );
	uint32_t				( *isCollapsed ) ( void* context, const char* label );
	void					( *setCollapsed ) ( void* context, const char* label, uint32_t isCollapsed );
	void					( *note ) ( void* context, const char* section, const char* parent, uint32_t isHidden );
	void					( *moveSection ) ( void* context, uint32_t from, uint32_t to );
} ReShadeUiPage;

typedef struct ReShadeWidgetTable
{
	uint32_t	bytes;

	// The look. beginFrame: once per drawn frame before anything of ours: the scale of this window, the glyph atlas on its device
	void		( *beginFrame ) ( void* runtime );
	float		( *scale ) ( void );
	float		( *screenScale ) ( void* window, float backBufferHeight );
	uint32_t	( *color ) ( uint32_t role );
	float		( *size ) ( uint32_t role );
	// The tuned values (the colours and sizes of res/panel-theme.json) by index: live, until the DLL unloads. themeText: the values as the
	// file's text (its length, written up to capacity); themeTake: a file's text taken in, 1 where it reads, else 0 with why
	uint32_t	( *tunedSizeCount ) ( void );
	const char*	( *tunedSizeName ) ( uint32_t index );
	float		( *tunedSizeValue ) ( uint32_t index );
	float		( *tunedSizeShipped ) ( uint32_t index );
	void		( *setTunedSize ) ( uint32_t index, float value );
	uint32_t	( *tunedColorCount ) ( void );
	const char*	( *tunedColorName ) ( uint32_t index );
	uint32_t	( *tunedColorValue ) ( uint32_t index );
	uint32_t	( *tunedColorShipped ) ( uint32_t index );
	void		( *setTunedColor ) ( uint32_t index, uint32_t color );
	uint32_t	( *themeText ) ( char* out, uint32_t capacity );
	uint32_t	( *themeTake ) ( const char* text, uint32_t length, char* why, uint32_t whyCapacity );

	// Text. draw: at x, y on the window being drawn, the width it took
	float		( *textWidth ) ( uint32_t face, float size, const char* text, uint32_t tabular );
	float		( *textDraw ) ( float x, float y, uint32_t face, float size, uint32_t color, const char* text, uint32_t tabular );
	float		( *lineHeight ) ( uint32_t face, float size );
	float		( *centredTop ) ( uint32_t face, float size, float top, float height );
	float		( *baselineInLine ) ( uint32_t face, float size, float lineHeight );
	void		( *placeLine ) ( uint32_t face, float size, const char* text, float maxWidth, ReShadeUiGlyph* glyphs, uint32_t capacity, ReShadeUiPlacedLine* out );
	uint32_t	( *rasterizeGlyph ) ( const ReShadeUiPlacedLine* line, const ReShadeUiGlyph* glyphs, uint32_t i, uint8_t* coverage, int32_t stride, int32_t rows, int32_t left, int32_t top );
	uint64_t	( *cachedGlyphBytes ) ( void );

	// Images by file path: a texture of the overlay's colour space (a mask: the file's alpha alone, white, height pixels tall), made once
	// per device and path; null while the file is missing or unreadable. decodeImage: the file's pixels as stored, R in the lowest
	// byte: the count of pixels it has, written up to capacity (0: unreadable)
	const void*	( *image ) ( void* runtime, const char* path );
	const void*	( *mask ) ( void* runtime, const char* path, int32_t height );
	uint32_t	( *decodeImage ) ( const char* path, uint32_t* rgba, uint32_t capacity, int32_t* width, int32_t* height );

	// The colour space the overlay is drawn in: the game's override, 0 none, else 1 sRGB, 2 scRGB, 3 HDR10, 4 HLG; shown while the swap
	// chain may be HDR or an override is set
	int32_t		( *colourSpaceOverride ) ( void );
	void		( *setColourSpaceOverride ) ( int32_t space );
	uint32_t	( *isColourSpaceShown ) ( void );

	// Widgets, each submits one ImGui item at the cursor, rowHeight tall unless noted, and knows nothing of settings. Results are
	// RESHADE_UI_CHANGED and RESHADE_UI_COMMITTED
	void		( *label ) ( float x, float y, const char* text, const char* tooltip, uint32_t applies );
	float		( *labelWidth ) ( const char* text, uint32_t hasTooltip, uint32_t applies );
	uint32_t	( *toggle ) ( const char* id, uint32_t* on, const char* text, const ReShadeUiToggleLook* look );
	void		( *heading ) ( const char* text, const ReShadeUiHotkey* hotkey );
	uint32_t	( *collapseArrow ) ( const char* id, uint32_t open, float width );
	void		( *subheading ) ( float x, float y, const char* text );
	uint32_t	( *slider ) ( const char* id, float* value, const ReShadeUiRange* range, float width );
	uint32_t	( *valueEdit ) ( const char* id, float* value, const ReShadeUiRange* range, const ReShadeUiFormat* format, float width );
	uint32_t	( *colorPicker ) ( const char* id, uint32_t* color, uint32_t def, float width );
	uint32_t	( *textField ) ( const char* id, char* text, uint32_t capacity, float width );
	uint32_t	( *resetButton ) ( const char* id );
	uint32_t	( *choice ) ( const char* id, int32_t* index, const char* const* names, uint32_t count, int32_t def, float width );
	uint32_t	( *dropdown ) ( const char* id, int32_t* index, const char* const* names, uint32_t count, int32_t def, float width );
	uint32_t	( *button ) ( const void* key, const char* text, uint32_t primary, const char* done, const ReShadeUiHotkey* hotkey, float progress );
	float		( *buttonWidth ) ( const void* key, const char* text, const char* done, const ReShadeUiHotkey* hotkey );
	void		( *flash ) ( const void* key );
	void		( *tooltip ) ( uint32_t id, uint32_t hovered, const char* text );
	void		( *header ) ( const ReShadeUiHeader* info, float width );
	int32_t		( *headerTitleHeight ) ( void );

	// A page: its header, sections and rows laid out and drawn as the whole of the overlay tab being drawn (a child of its own, scrolled
	// and filling it), the frame handed to every callback of the page. isMouseOver: the mouse was over ultraReShade's overlay at the last
	// frame (its own input, not the game's); any thread
	void		( *pageDraw ) ( void* runtime, const ReShadeUiPage* page, void* frame );
	uint32_t	( *isMouseOver ) ( void );

	// A tab's name gets a dot after it in that colour, 0 for none (the tab by the title its overlay was registered with); text is being
	// typed into a field of the overlay: the add-on's own keys stay silent meanwhile. Any thread
	void		( *setTabDot ) ( const char* title, uint32_t color );
	uint32_t	( *isTyping ) ( void );
} ReShadeWidgetTable;

// The table, or null where its size is less than bytes (the DLL is older than the caller)
typedef const ReShadeWidgetTable* ( *ReShadeGetWidgetTableFn ) ( uint32_t bytes );
#define RESHADE_UI_TABLE_EXPORT	"ReShadeGetWidgetTable"

#ifdef __cplusplus
}
#endif
