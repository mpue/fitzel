#pragma once

// The C# editor's text widget: ImGuiColorTextEdit's TextEditor (BalazsJako,
// MIT -- licence below), taken in and grown into what an IDE editor needs:
//
//   - semantic colours laid over the lexer's (types, methods, locals... from
//     the language service), kept per glyph so they follow every edit;
//   - squiggles under errors and warnings, per glyph too;
//   - edits from outside (completion, format, rename) that undo like typing,
//     several at once undoing as one step;
//   - typing help: closing brackets and quotes, indent after `{`, outdent on
//     `}`, spaces for tabs, Ctrl+/ comments, smart Home, word deletion;
//   - bracket matching, highlighted ranges (find, references);
//   - text <-> screen positions for popups, hover and Ctrl+click;
//   - a hand-written C# lexer (no std::regex: that is slow in MSVC's library).
//
// The Lua editor keeps the original widget (ScriptEditor.hpp); this one is
// only the C# editor's (CSharpEditor.hpp). Editor only.
//
// ---------------------------------------------------------------------------
// MIT License -- Copyright (c) 2017 BalazsJako
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------

#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"

class CodeEditor
{
public:
	enum class PaletteIndex : uint8_t
	{
		Default,
		Keyword,
		Number,
		String,
		CharLiteral,
		Punctuation,
		Preprocessor,
		Identifier,
		KnownIdentifier,
		PreprocIdentifier,
		Comment,
		MultiLineComment,
		Background,
		Cursor,
		Selection,
		ErrorMarker,
		Breakpoint,
		LineNumber,
		CurrentLineFill,
		CurrentLineFillInactive,
		CurrentLineEdge,
		// Semantic colours (SetSemanticTokens)
		Type,
		Method,
		Member,
		Variable,
		EnumMember,
		Namespace,
		ControlKeyword,
		ExcludedCode,
		StringEscape,
		// Decorations
		ErrorSquiggle,
		WarningSquiggle,
		InfoSquiggle,
		Highlight,
		BracketMatch,
		Max
	};

	enum class SelectionMode
	{
		Normal,
		Word,
		Line
	};

	// Represents a character coordinate from the user's point of view,
	// i. e. consider an uniform grid (assuming fixed-width font) on the
	// screen as it is rendered, and each cell has its own coordinate, starting from 0.
	// Tabs are counted as [1..mTabSize] count empty spaces, depending on
	// how many space is necessary to reach the next tab stop.
	// For example, coordinate (1, 5) represents the character 'B' in a line "\tABC", when mTabSize = 4,
	// because it is rendered as "    ABC" on the screen.
	struct Coordinates
	{
		int mLine, mColumn;
		Coordinates() : mLine(0), mColumn(0) {}
		Coordinates(int aLine, int aColumn) : mLine(aLine), mColumn(aColumn)
		{
			assert(aLine >= 0);
			assert(aColumn >= 0);
		}
		static Coordinates Invalid() { static Coordinates invalid(-1, -1); return invalid; }

		bool operator ==(const Coordinates& o) const { return mLine == o.mLine && mColumn == o.mColumn; }
		bool operator !=(const Coordinates& o) const { return mLine != o.mLine || mColumn != o.mColumn; }
		bool operator <(const Coordinates& o) const { return mLine != o.mLine ? mLine < o.mLine : mColumn < o.mColumn; }
		bool operator >(const Coordinates& o) const { return mLine != o.mLine ? mLine > o.mLine : mColumn > o.mColumn; }
		bool operator <=(const Coordinates& o) const { return mLine != o.mLine ? mLine < o.mLine : mColumn <= o.mColumn; }
		bool operator >=(const Coordinates& o) const { return mLine != o.mLine ? mLine > o.mLine : mColumn >= o.mColumn; }
	};

	struct Identifier
	{
		Coordinates mLocation;
		std::string mDeclaration;
	};

	typedef std::string String;
	typedef std::unordered_map<std::string, Identifier> Identifiers;
	typedef std::unordered_set<std::string> Keywords;
	typedef std::array<ImU32, (unsigned)PaletteIndex::Max> Palette;
	typedef uint8_t Char;

	struct Glyph
	{
		Char mChar;
		PaletteIndex mColorIndex = PaletteIndex::Default;
		PaletteIndex mSemantic = PaletteIndex::Default;   // from the language service
		uint8_t mDiag = 0;                                 // squiggle: 0 none, 1 info, 2 warning, 3 error
		bool mComment : 1;
		bool mMultiLineComment : 1;
		bool mPreprocessor : 1;

		Glyph(Char aChar, PaletteIndex aColorIndex) : mChar(aChar), mColorIndex(aColorIndex),
			mComment(false), mMultiLineComment(false), mPreprocessor(false) {}
	};

	typedef std::vector<Glyph> Line;
	typedef std::vector<Line> Lines;

	struct LanguageDefinition
	{
		typedef bool(*TokenizeCallback)(const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end, PaletteIndex & paletteIndex);

		std::string mName;
		Keywords mKeywords;
		Keywords mControlKeywords;
		Identifiers mIdentifiers;
		Identifiers mPreprocIdentifiers;
		std::string mCommentStart, mCommentEnd, mSingleLineComment;
		char mPreprocChar;
		bool mAutoIndentation;

		TokenizeCallback mTokenize;

		bool mCaseSensitive;

		LanguageDefinition()
			: mPreprocChar('#'), mAutoIndentation(true), mTokenize(nullptr), mCaseSensitive(true)
		{
		}

		static const LanguageDefinition& CSharp();
	};

	// A range in {line, ch} terms: ch in characters (code points) from the
	// line's start, a tab counting one -- the language service's positions.
	struct TextPos { int line = 0, ch = 0; };

	CodeEditor();
	~CodeEditor();

	void SetLanguageDefinition(const LanguageDefinition& aLanguageDef);
	const LanguageDefinition& GetLanguageDefinition() const { return mLanguageDefinition; }

	const Palette& GetPalette() const { return mPaletteBase; }
	void SetPalette(const Palette& aValue);

	void Render(const char* aTitle, const ImVec2& aSize = ImVec2(), bool aBorder = false);
	void SetText(const std::string& aText);
	std::string GetText() const;

	void SetTextLines(const std::vector<std::string>& aLines);
	std::vector<std::string> GetTextLines() const;

	std::string GetSelectedText() const;
	std::string GetCurrentLineText()const;
	std::string GetLineText(int aLine) const;

	int GetTotalLines() const { return (int)mLines.size(); }
	bool IsOverwrite() const { return mOverwrite; }

	void SetReadOnly(bool aValue);
	bool IsReadOnly() const { return mReadOnly; }
	bool IsTextChanged() const { return mTextChanged; }
	bool IsCursorPositionChanged() const { return mCursorPositionChanged; }

	bool IsColorizerEnabled() const { return mColorizerEnabled; }
	void SetColorizerEnable(bool aValue);

	Coordinates GetCursorPosition() const { return GetActualCursorCoordinates(); }
	void SetCursorPosition(const Coordinates& aPosition);
	// Put the caret there and scroll it to the middle of the view (jumps).
	// aFocus: the widget takes the keyboard (false: a find field keeps it).
	void JumpTo(const Coordinates& aPosition, bool aFocus = true);

	inline void SetHandleMouseInputs    (bool aValue){ mHandleMouseInputs    = aValue;}
	inline bool IsHandleMouseInputsEnabled() const { return mHandleMouseInputs; }

	inline void SetHandleKeyboardInputs (bool aValue){ mHandleKeyboardInputs = aValue;}
	inline bool IsHandleKeyboardInputsEnabled() const { return mHandleKeyboardInputs; }

	inline void SetImGuiChildIgnored    (bool aValue){ mIgnoreImGuiChild     = aValue;}
	inline bool IsImGuiChildIgnored() const { return mIgnoreImGuiChild; }

	inline void SetShowWhitespaces(bool aValue) { mShowWhitespaces = aValue; }
	inline bool IsShowingWhitespaces() const { return mShowWhitespaces; }

	void SetTabSize(int aValue);
	inline int GetTabSize() const { return mTabSize; }
	// Tab inserts spaces (to the next tab stop) instead of a tab character.
	inline void SetInsertSpaces(bool aValue) { mInsertSpaces = aValue; }

	void InsertText(const std::string& aValue);
	void InsertText(const char* aValue);

	void MoveUp(int aAmount = 1, bool aSelect = false);
	void MoveDown(int aAmount = 1, bool aSelect = false);
	void MoveLeft(int aAmount = 1, bool aSelect = false, bool aWordMode = false);
	void MoveRight(int aAmount = 1, bool aSelect = false, bool aWordMode = false);
	void MoveTop(bool aSelect = false);
	void MoveBottom(bool aSelect = false);
	void MoveHome(bool aSelect = false);
	void MoveEnd(bool aSelect = false);

	void SetSelectionStart(const Coordinates& aPosition);
	void SetSelectionEnd(const Coordinates& aPosition);
	void SetSelection(const Coordinates& aStart, const Coordinates& aEnd, SelectionMode aMode = SelectionMode::Normal);
	void SelectWordUnderCursor();
	void SelectAll();
	bool HasSelection() const;
	Coordinates GetSelectionStart() const { return mState.mSelectionStart; }
	Coordinates GetSelectionEnd() const { return mState.mSelectionEnd; }

	void Copy();
	void Cut();
	void Paste();
	void Delete();

	bool CanUndo() const;
	bool CanRedo() const;
	void Undo(int aSteps = 1);
	void Redo(int aSteps = 1);

	// --- What the C# editor added ------------------------------------------------
	// Replace [start, end) with text, undoable; the caret ends after the text.
	void ReplaceRange(const Coordinates& aStart, const Coordinates& aEnd, const std::string& aText);
	// Everything between Begin and End undoes (and redoes) as one step.
	void BeginUndoGroup();
	void EndUndoGroup();
	// Toggle // on the selected lines (or the caret's).
	void ToggleLineComment();

	// {line, ch} <-> the widget's (visual) coordinates.
	Coordinates FromTextPos(int aLine, int aCh) const;
	TextPos ToTextPos(const Coordinates& aCoords) const;

	// Semantic colours: a flat {line, ch, length, colour} list. Replaces the
	// previous set.
	struct SemanticToken { int line, ch, len; PaletteIndex color; };
	void SetSemanticTokens(const std::vector<SemanticToken>& aTokens);
	// Squiggles: replaces the previous set. Severity 1 info, 2 warning, 3 error.
	struct Squiggle { TextPos start, end; int severity; };
	void SetSquiggles(const std::vector<Squiggle>& aSquiggles);
	// Ranges drawn behind the text (find matches, references).
	void SetHighlights(const std::vector<std::pair<Coordinates, Coordinates>>& aRanges) { mHighlights = aRanges; }

	// Screen position of a coordinate's cell (top left), valid after Render.
	ImVec2 CoordinatesToScreen(const Coordinates& aCoords) const;
	// The coordinate under a screen point (after Render); false when the point
	// is outside the text area or past the end of its line.
	bool ScreenToCoordinates(const ImVec2& aPos, Coordinates& aOut) const;
	float GetLineHeight() const { return mCharAdvance.y; }
	float GetCharWidth() const { return mCharAdvance.x; }
	// The text area's screen rectangle (after Render).
	ImVec2 GetViewMin() const { return mViewMin; }
	ImVec2 GetViewMax() const { return mViewMax; }
	// The widget had the keyboard focus during the last Render.
	bool IsFocused() const { return mFocused; }
	// The characters typed during the last Render (ASCII), and whether a
	// Backspace/Delete happened.
	const std::string& GetTypedChars() const { return mTyped; }
	bool WasDeleting() const { return mDeleted; }

	// The identifier at (or just before) a coordinate: its start and end. False
	// when there is none.
	bool GetIdentifierAt(const Coordinates& aAt, Coordinates& aStart, Coordinates& aEnd) const;

	static const Palette& GetDarkPalette();

private:
	struct EditorState
	{
		Coordinates mSelectionStart;
		Coordinates mSelectionEnd;
		Coordinates mCursorPosition;
	};

	class UndoRecord
	{
	public:
		UndoRecord() {}
		~UndoRecord() {}

		UndoRecord(
			const std::string& aAdded,
			const CodeEditor::Coordinates aAddedStart,
			const CodeEditor::Coordinates aAddedEnd,

			const std::string& aRemoved,
			const CodeEditor::Coordinates aRemovedStart,
			const CodeEditor::Coordinates aRemovedEnd,

			CodeEditor::EditorState& aBefore,
			CodeEditor::EditorState& aAfter);

		void Undo(CodeEditor* aEditor);
		void Redo(CodeEditor* aEditor);

		std::string mAdded;
		Coordinates mAddedStart;
		Coordinates mAddedEnd;

		std::string mRemoved;
		Coordinates mRemovedStart;
		Coordinates mRemovedEnd;

		EditorState mBefore;
		EditorState mAfter;
		int mGroup = 0;   // records of one group undo together (0 = alone)
	};

	typedef std::vector<UndoRecord> UndoBuffer;

	void Colorize(int aFromLine = 0, int aCount = -1);
	void ColorizeRange(int aFromLine = 0, int aToLine = 0);
	void ColorizeInternal();
	float TextDistanceToLineStart(const Coordinates& aFrom) const;
	void EnsureCursorVisible();
	int GetPageSize() const;
	std::string GetText(const Coordinates& aStart, const Coordinates& aEnd) const;
	Coordinates GetActualCursorCoordinates() const;
	Coordinates SanitizeCoordinates(const Coordinates& aValue) const;
	void Advance(Coordinates& aCoordinates) const;
	void DeleteRange(const Coordinates& aStart, const Coordinates& aEnd);
	int InsertTextAt(Coordinates& aWhere, const char* aValue);
	void AddUndo(UndoRecord& aValue);
	Coordinates ScreenPosToCoordinates(const ImVec2& aPosition) const;
	Coordinates FindWordStart(const Coordinates& aFrom) const;
	Coordinates FindWordEnd(const Coordinates& aFrom) const;
	Coordinates FindNextWord(const Coordinates& aFrom) const;
	int GetCharacterIndex(const Coordinates& aCoordinates) const;
	int GetCharacterColumn(int aLine, int aIndex) const;
	int GetLineCharacterCount(int aLine) const;
	int GetLineMaxColumn(int aLine) const;
	bool IsOnWordBoundary(const Coordinates& aAt) const;
	void RemoveLine(int aStart, int aEnd);
	void RemoveLine(int aIndex);
	Line& InsertLine(int aIndex);
	void EnterCharacter(ImWchar aChar, bool aShift);
	void TypeCharacter(ImWchar aChar, bool aShift);
	void IndentLines(bool aOutdent);
	void Backspace();
	void DeleteSelection();
	std::string GetWordUnderCursor() const;
	std::string GetWordAt(const Coordinates& aCoords) const;
	ImU32 GetGlyphColor(const Glyph& aGlyph) const;
	bool IsCodeGlyph(const Glyph& aGlyph) const;
	bool FindMatchingBracket(const Coordinates& aAt, Coordinates& aOpen, Coordinates& aClose) const;
	int ByteOfChar(int aLine, int aCh) const;
	ImFont* Font() const { return mFont ? mFont : ImGui::GetFont(); }
	float FontSize() const { return mFont ? mFontSize : ImGui::GetFontSize(); }

	void HandleKeyboardInputs();
	void HandleMouseInputs();
	void Render();

	float mLineSpacing;
	Lines mLines;
	EditorState mState;
	UndoBuffer mUndoBuffer;
	int mUndoIndex;
	int mUndoGroupDepth = 0;
	int mUndoGroupId = 0;
	int mUndoGroupCounter = 0;

	int mTabSize;
	bool mInsertSpaces = true;
	bool mOverwrite;
	bool mReadOnly;
	bool mWithinRender;
	bool mScrollToCursor;
	bool mCenterOnCursor = false;
	bool mStealFocus = true;            // scrolling to the caret focuses the widget
	bool mScrollToTop;
	bool mTextChanged;
	bool mColorizerEnabled;
	float mTextStart;                   // position (in pixels) where a code line starts relative to the left of the CodeEditor.
	int  mLeftMargin;
	bool mCursorPositionChanged;
	int mColorRangeMin, mColorRangeMax;
	SelectionMode mSelectionMode;
	bool mHandleKeyboardInputs;
	bool mHandleMouseInputs;
	bool mIgnoreImGuiChild;
	bool mShowWhitespaces;

	Palette mPaletteBase;
	Palette mPalette;
	LanguageDefinition mLanguageDefinition;

	bool mCheckComments;
	ImVec2 mCharAdvance;
	Coordinates mInteractiveStart, mInteractiveEnd;
	std::string mLineBuffer;
	uint64_t mStartTime;

	float mLastClick;

	// Added state
	ImFont* mFont = nullptr;     // the font of the last Render (positions outside it)
	float   mFontSize = 13.0f;
	ImVec2  mOrigin;             // screen position of line 0, column 0 (scrolled)
	ImVec2  mViewMin, mViewMax;
	bool    mFocused = false;
	std::string mTyped;
	bool    mDeleted = false;
	std::vector<std::pair<Coordinates, Coordinates>> mHighlights;
};
