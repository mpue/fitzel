// The C# editor's text widget: see CodeEditor.hpp (a fork of ImGuiColorTextEdit,
// MIT, Copyright (c) 2017 BalazsJako -- the licence is in the header).
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "CodeEditor.hpp"

#include "imgui.h"

// TODO
// - multiline comments vs single-line: latter is blocking start of a ML

template<class InputIt1, class InputIt2, class BinaryPredicate>
bool equals(InputIt1 first1, InputIt1 last1,
	InputIt2 first2, InputIt2 last2, BinaryPredicate p)
{
	for (; first1 != last1 && first2 != last2; ++first1, ++first2)
	{
		if (!p(*first1, *first2))
			return false;
	}
	return first1 == last1 && first2 == last2;
}

CodeEditor::CodeEditor()
	: mLineSpacing(1.0f)
	, mUndoIndex(0)
	, mTabSize(4)
	, mOverwrite(false)
	, mReadOnly(false)
	, mWithinRender(false)
	, mScrollToCursor(false)
	, mScrollToTop(false)
	, mTextChanged(false)
	, mColorizerEnabled(true)
	, mTextStart(20.0f)
	, mLeftMargin(10)
	, mCursorPositionChanged(false)
	, mColorRangeMin(0)
	, mColorRangeMax(0)
	, mSelectionMode(SelectionMode::Normal)
	, mCheckComments(true)
	, mLastClick(-1.0f)
	, mHandleKeyboardInputs(true)
	, mHandleMouseInputs(true)
	, mIgnoreImGuiChild(false)
	, mShowWhitespaces(false)
	, mStartTime(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())
{
	SetPalette(GetDarkPalette());
	SetLanguageDefinition(LanguageDefinition::CSharp());
	mLines.push_back(Line());
}

CodeEditor::~CodeEditor()
{
}

void CodeEditor::SetLanguageDefinition(const LanguageDefinition & aLanguageDef)
{
	mLanguageDefinition = aLanguageDef;
	Colorize();
}

void CodeEditor::SetPalette(const Palette & aValue)
{
	mPaletteBase = aValue;
}

std::string CodeEditor::GetText(const Coordinates & aStart, const Coordinates & aEnd) const
{
	std::string result;

	auto lstart = aStart.mLine;
	auto lend = aEnd.mLine;
	auto istart = GetCharacterIndex(aStart);
	auto iend = GetCharacterIndex(aEnd);
	size_t s = 0;

	for (size_t i = lstart; i < lend; i++)
		s += mLines[i].size();

	result.reserve(s + s / 8);

	while (istart < iend || lstart < lend)
	{
		if (lstart >= (int)mLines.size())
			break;

		auto& line = mLines[lstart];
		if (istart < (int)line.size())
		{
			result += line[istart].mChar;
			istart++;
		}
		else
		{
			istart = 0;
			++lstart;
			result += '\n';
		}
	}

	return result;
}

CodeEditor::Coordinates CodeEditor::GetActualCursorCoordinates() const
{
	return SanitizeCoordinates(mState.mCursorPosition);
}

CodeEditor::Coordinates CodeEditor::SanitizeCoordinates(const Coordinates & aValue) const
{
	auto line = aValue.mLine;
	auto column = aValue.mColumn;
	if (line >= (int)mLines.size())
	{
		if (mLines.empty())
		{
			line = 0;
			column = 0;
		}
		else
		{
			line = (int)mLines.size() - 1;
			column = GetLineMaxColumn(line);
		}
		return Coordinates(line, column);
	}
	else
	{
		column = mLines.empty() ? 0 : std::min(column, GetLineMaxColumn(line));
		return Coordinates(line, column);
	}
}

// https://en.wikipedia.org/wiki/UTF-8
// We assume that the char is a standalone character (<128) or a leading byte of an UTF-8 code sequence (non-10xxxxxx code)
static int UTF8CharLength(CodeEditor::Char c)
{
	if ((c & 0xFE) == 0xFC)
		return 6;
	if ((c & 0xFC) == 0xF8)
		return 5;
	if ((c & 0xF8) == 0xF0)
		return 4;
	else if ((c & 0xF0) == 0xE0)
		return 3;
	else if ((c & 0xE0) == 0xC0)
		return 2;
	return 1;
}

// "Borrowed" from ImGui source
static inline int ImTextCharToUtf8(char* buf, int buf_size, unsigned int c)
{
	if (c < 0x80)
	{
		buf[0] = (char)c;
		return 1;
	}
	if (c < 0x800)
	{
		if (buf_size < 2) return 0;
		buf[0] = (char)(0xc0 + (c >> 6));
		buf[1] = (char)(0x80 + (c & 0x3f));
		return 2;
	}
	if (c >= 0xdc00 && c < 0xe000)
	{
		return 0;
	}
	if (c >= 0xd800 && c < 0xdc00)
	{
		if (buf_size < 4) return 0;
		buf[0] = (char)(0xf0 + (c >> 18));
		buf[1] = (char)(0x80 + ((c >> 12) & 0x3f));
		buf[2] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[3] = (char)(0x80 + ((c) & 0x3f));
		return 4;
	}
	//else if (c < 0x10000)
	{
		if (buf_size < 3) return 0;
		buf[0] = (char)(0xe0 + (c >> 12));
		buf[1] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[2] = (char)(0x80 + ((c) & 0x3f));
		return 3;
	}
}

void CodeEditor::Advance(Coordinates & aCoordinates) const
{
	if (aCoordinates.mLine < (int)mLines.size())
	{
		auto& line = mLines[aCoordinates.mLine];
		auto cindex = GetCharacterIndex(aCoordinates);

		if (cindex + 1 < (int)line.size())
		{
			auto delta = UTF8CharLength(line[cindex].mChar);
			cindex = std::min(cindex + delta, (int)line.size() - 1);
		}
		else
		{
			++aCoordinates.mLine;
			cindex = 0;
		}
		aCoordinates.mColumn = GetCharacterColumn(aCoordinates.mLine, cindex);
	}
}

void CodeEditor::DeleteRange(const Coordinates & aStart, const Coordinates & aEnd)
{
	assert(aEnd >= aStart);
	assert(!mReadOnly);

	//printf("D(%d.%d)-(%d.%d)\n", aStart.mLine, aStart.mColumn, aEnd.mLine, aEnd.mColumn);

	if (aEnd == aStart)
		return;

	auto start = GetCharacterIndex(aStart);
	auto end = GetCharacterIndex(aEnd);

	if (aStart.mLine == aEnd.mLine)
	{
		auto& line = mLines[aStart.mLine];
		auto n = GetLineMaxColumn(aStart.mLine);
		if (aEnd.mColumn >= n)
			line.erase(line.begin() + start, line.end());
		else
			line.erase(line.begin() + start, line.begin() + end);
	}
	else
	{
		auto& firstLine = mLines[aStart.mLine];
		auto& lastLine = mLines[aEnd.mLine];

		firstLine.erase(firstLine.begin() + start, firstLine.end());
		lastLine.erase(lastLine.begin(), lastLine.begin() + end);

		if (aStart.mLine < aEnd.mLine)
			firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());

		if (aStart.mLine < aEnd.mLine)
			RemoveLine(aStart.mLine + 1, aEnd.mLine + 1);
	}

	mTextChanged = true;
}

int CodeEditor::InsertTextAt(Coordinates& /* inout */ aWhere, const char * aValue)
{
	assert(!mReadOnly);

	int cindex = GetCharacterIndex(aWhere);
	int totalLines = 0;
	while (*aValue != '\0')
	{
		assert(!mLines.empty());

		if (*aValue == '\r')
		{
			// skip
			++aValue;
		}
		else if (*aValue == '\n')
		{
			if (cindex < (int)mLines[aWhere.mLine].size())
			{
				auto& newLine = InsertLine(aWhere.mLine + 1);
				auto& line = mLines[aWhere.mLine];
				newLine.insert(newLine.begin(), line.begin() + cindex, line.end());
				line.erase(line.begin() + cindex, line.end());
			}
			else
			{
				InsertLine(aWhere.mLine + 1);
			}
			++aWhere.mLine;
			aWhere.mColumn = 0;
			cindex = 0;
			++totalLines;
			++aValue;
		}
		else
		{
			auto& line = mLines[aWhere.mLine];
			auto d = UTF8CharLength(*aValue);
			while (d-- > 0 && *aValue != '\0')
				line.insert(line.begin() + cindex++, Glyph(*aValue++, PaletteIndex::Default));
			aWhere.mColumn = GetCharacterColumn(aWhere.mLine, cindex);
		}

		mTextChanged = true;
	}

	return totalLines;
}

void CodeEditor::AddUndo(UndoRecord& aValue)
{
	assert(!mReadOnly);
	//printf("AddUndo: (@%d.%d) +\'%s' [%d.%d .. %d.%d], -\'%s', [%d.%d .. %d.%d] (@%d.%d)\n",
	//	aValue.mBefore.mCursorPosition.mLine, aValue.mBefore.mCursorPosition.mColumn,
	//	aValue.mAdded.c_str(), aValue.mAddedStart.mLine, aValue.mAddedStart.mColumn, aValue.mAddedEnd.mLine, aValue.mAddedEnd.mColumn,
	//	aValue.mRemoved.c_str(), aValue.mRemovedStart.mLine, aValue.mRemovedStart.mColumn, aValue.mRemovedEnd.mLine, aValue.mRemovedEnd.mColumn,
	//	aValue.mAfter.mCursorPosition.mLine, aValue.mAfter.mCursorPosition.mColumn
	//	);

	mUndoBuffer.resize((size_t)(mUndoIndex + 1));
	mUndoBuffer.back() = aValue;
	mUndoBuffer.back().mGroup = mUndoGroupDepth > 0 ? mUndoGroupId : 0;
	++mUndoIndex;
}

CodeEditor::Coordinates CodeEditor::ScreenPosToCoordinates(const ImVec2& aPosition) const
{
	ImVec2 origin = mWithinRender ? ImGui::GetCursorScreenPos() : mOrigin;
	ImVec2 local(aPosition.x - origin.x, aPosition.y - origin.y);

	int lineNo = std::max(0, (int)floor(local.y / mCharAdvance.y));

	int columnCoord = 0;

	if (lineNo >= 0 && lineNo < (int)mLines.size())
	{
		auto& line = mLines.at(lineNo);

		int columnIndex = 0;
		float columnX = 0.0f;

		while ((size_t)columnIndex < line.size())
		{
			float columnWidth = 0.0f;

			if (line[columnIndex].mChar == '\t')
			{
				float spaceSize = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, " ").x;
				float oldX = columnX;
				float newColumnX = (1.0f + std::floor((1.0f + columnX) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
				columnWidth = newColumnX - oldX;
				if (mTextStart + columnX + columnWidth * 0.5f > local.x)
					break;
				columnX = newColumnX;
				columnCoord = (columnCoord / mTabSize) * mTabSize + mTabSize;
				columnIndex++;
			}
			else
			{
				char buf[7];
				auto d = UTF8CharLength(line[columnIndex].mChar);
				int i = 0;
				while (i < 6 && d-- > 0)
					buf[i++] = line[columnIndex++].mChar;
				buf[i] = '\0';
				columnWidth = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, buf).x;
				if (mTextStart + columnX + columnWidth * 0.5f > local.x)
					break;
				columnX += columnWidth;
				columnCoord++;
			}
		}
	}

	return SanitizeCoordinates(Coordinates(lineNo, columnCoord));
}

CodeEditor::Coordinates CodeEditor::FindWordStart(const Coordinates & aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
		return at;

	auto& line = mLines[at.mLine];
	auto cindex = GetCharacterIndex(at);

	if (cindex >= (int)line.size())
		return at;

	while (cindex > 0 && isspace(line[cindex].mChar))
		--cindex;

	auto cstart = (PaletteIndex)line[cindex].mColorIndex;
	while (cindex > 0)
	{
		auto c = line[cindex].mChar;
		if ((c & 0xC0) != 0x80)	// not UTF code sequence 10xxxxxx
		{
			if (c <= 32 && isspace(c))
			{
				cindex++;
				break;
			}
			if (cstart != (PaletteIndex)line[size_t(cindex - 1)].mColorIndex)
				break;
		}
		--cindex;
	}
	return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));
}

CodeEditor::Coordinates CodeEditor::FindWordEnd(const Coordinates & aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
		return at;

	auto& line = mLines[at.mLine];
	auto cindex = GetCharacterIndex(at);

	if (cindex >= (int)line.size())
		return at;

	bool prevspace = isspace(line[cindex].mChar) != 0;
	auto cstart = (PaletteIndex)line[cindex].mColorIndex;
	while (cindex < (int)line.size())
	{
		auto c = line[cindex].mChar;
		auto d = UTF8CharLength(c);
		if (cstart != (PaletteIndex)line[cindex].mColorIndex)
			break;

		if (prevspace != !!isspace(c))
		{
			if (isspace(c))
				while (cindex < (int)line.size() && isspace(line[cindex].mChar))
					++cindex;
			break;
		}
		cindex += d;
	}
	return Coordinates(aFrom.mLine, GetCharacterColumn(aFrom.mLine, cindex));
}

CodeEditor::Coordinates CodeEditor::FindNextWord(const Coordinates & aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
		return at;

	// skip to the next non-word character
	auto cindex = GetCharacterIndex(aFrom);
	bool isword = false;
	bool skip = false;
	if (cindex < (int)mLines[at.mLine].size())
	{
		auto& line = mLines[at.mLine];
		isword = isalnum(line[cindex].mChar) != 0;
		skip = isword;
	}

	while (!isword || skip)
	{
		if (at.mLine >= mLines.size())
		{
			auto l = std::max(0, (int) mLines.size() - 1);
			return Coordinates(l, GetLineMaxColumn(l));
		}

		auto& line = mLines[at.mLine];
		if (cindex < (int)line.size())
		{
			isword = isalnum(line[cindex].mChar) != 0;

			if (isword && !skip)
				return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));

			if (!isword)
				skip = false;

			cindex++;
		}
		else
		{
			cindex = 0;
			++at.mLine;
			skip = false;
			isword = false;
		}
	}

	return at;
}

int CodeEditor::GetCharacterIndex(const Coordinates& aCoordinates) const
{
	if (aCoordinates.mLine >= mLines.size())
		return -1;
	auto& line = mLines[aCoordinates.mLine];
	int c = 0;
	int i = 0;
	for (; i < line.size() && c < aCoordinates.mColumn;)
	{
		if (line[i].mChar == '\t')
			c = (c / mTabSize) * mTabSize + mTabSize;
		else
			++c;
		i += UTF8CharLength(line[i].mChar);
	}
	return i;
}

int CodeEditor::GetCharacterColumn(int aLine, int aIndex) const
{
	if (aLine >= mLines.size())
		return 0;
	auto& line = mLines[aLine];
	int col = 0;
	int i = 0;
	while (i < aIndex && i < (int)line.size())
	{
		auto c = line[i].mChar;
		i += UTF8CharLength(c);
		if (c == '\t')
			col = (col / mTabSize) * mTabSize + mTabSize;
		else
			col++;
	}
	return col;
}

int CodeEditor::GetLineCharacterCount(int aLine) const
{
	if (aLine >= mLines.size())
		return 0;
	auto& line = mLines[aLine];
	int c = 0;
	for (unsigned i = 0; i < line.size(); c++)
		i += UTF8CharLength(line[i].mChar);
	return c;
}

int CodeEditor::GetLineMaxColumn(int aLine) const
{
	if (aLine >= mLines.size())
		return 0;
	auto& line = mLines[aLine];
	int col = 0;
	for (unsigned i = 0; i < line.size(); )
	{
		auto c = line[i].mChar;
		if (c == '\t')
			col = (col / mTabSize) * mTabSize + mTabSize;
		else
			col++;
		i += UTF8CharLength(c);
	}
	return col;
}

bool CodeEditor::IsOnWordBoundary(const Coordinates & aAt) const
{
	if (aAt.mLine >= (int)mLines.size() || aAt.mColumn == 0)
		return true;

	auto& line = mLines[aAt.mLine];
	auto cindex = GetCharacterIndex(aAt);
	if (cindex >= (int)line.size())
		return true;

	if (mColorizerEnabled)
		return line[cindex].mColorIndex != line[size_t(cindex - 1)].mColorIndex;

	return isspace(line[cindex].mChar) != isspace(line[cindex - 1].mChar);
}

void CodeEditor::RemoveLine(int aStart, int aEnd)
{
	assert(!mReadOnly);
	assert(aEnd >= aStart);
	assert(mLines.size() > (size_t)(aEnd - aStart));

	mLines.erase(mLines.begin() + aStart, mLines.begin() + aEnd);
	assert(!mLines.empty());

	mTextChanged = true;
}

void CodeEditor::RemoveLine(int aIndex)
{
	assert(!mReadOnly);
	assert(mLines.size() > 1);

	mLines.erase(mLines.begin() + aIndex);
	assert(!mLines.empty());

	mTextChanged = true;
}

CodeEditor::Line& CodeEditor::InsertLine(int aIndex)
{
	assert(!mReadOnly);

	auto& result = *mLines.insert(mLines.begin() + aIndex, Line());

	return result;
}

std::string CodeEditor::GetWordUnderCursor() const
{
	auto c = GetCursorPosition();
	return GetWordAt(c);
}

std::string CodeEditor::GetWordAt(const Coordinates & aCoords) const
{
	auto start = FindWordStart(aCoords);
	auto end = FindWordEnd(aCoords);

	std::string r;

	auto istart = GetCharacterIndex(start);
	auto iend = GetCharacterIndex(end);

	for (auto it = istart; it < iend; ++it)
		r.push_back(mLines[aCoords.mLine][it].mChar);

	return r;
}

ImU32 CodeEditor::GetGlyphColor(const Glyph & aGlyph) const
{
	if (!mColorizerEnabled)
		return mPalette[(int)PaletteIndex::Default];
	if (aGlyph.mSemantic != PaletteIndex::Default && (!(aGlyph.mComment || aGlyph.mMultiLineComment) ||
	                                                   aGlyph.mSemantic == PaletteIndex::ExcludedCode))
		return mPalette[(int)aGlyph.mSemantic];
	if (aGlyph.mComment)
		return mPalette[(int)PaletteIndex::Comment];
	if (aGlyph.mMultiLineComment)
		return mPalette[(int)PaletteIndex::MultiLineComment];
	auto const color = mPalette[(int)aGlyph.mColorIndex];
	if (aGlyph.mPreprocessor)
	{
		const auto ppcolor = mPalette[(int)PaletteIndex::Preprocessor];
		const int c0 = ((ppcolor & 0xff) + (color & 0xff)) / 2;
		const int c1 = (((ppcolor >> 8) & 0xff) + ((color >> 8) & 0xff)) / 2;
		const int c2 = (((ppcolor >> 16) & 0xff) + ((color >> 16) & 0xff)) / 2;
		const int c3 = (((ppcolor >> 24) & 0xff) + ((color >> 24) & 0xff)) / 2;
		return ImU32(c0 | (c1 << 8) | (c2 << 16) | (c3 << 24));
	}
	return color;
}

void CodeEditor::HandleKeyboardInputs()
{
	ImGuiIO& io = ImGui::GetIO();
	auto shift = io.KeyShift;
	auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

	if (ImGui::IsWindowFocused())
	{
		if (ImGui::IsWindowHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

		io.WantCaptureKeyboard = true;
		io.WantTextInput = true;

		if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Z))
			Undo();
		else if (!IsReadOnly() && ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Z))
			Redo();
		else if (!IsReadOnly() && !ctrl && !shift && alt && ImGui::IsKeyPressed(ImGuiKey_Backspace))
			Undo();
		else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Y))
			Redo();
		// Ctrl+/ (US), Ctrl+# (German: the same physical key as US backslash), keypad /
		else if (!IsReadOnly() && ctrl && !alt &&
		         (ImGui::IsKeyPressed(ImGuiKey_Slash) || ImGui::IsKeyPressed(ImGuiKey_Backslash) ||
		          ImGui::IsKeyPressed(ImGuiKey_KeypadDivide)))
			ToggleLineComment();
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			MoveUp(1, shift);
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			MoveDown(1, shift);
		else if (!alt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
			MoveLeft(1, shift, ctrl);
		else if (!alt && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
			MoveRight(1, shift, ctrl);
		else if (!alt && ImGui::IsKeyPressed(ImGuiKey_PageUp))
			MoveUp(GetPageSize() - 4, shift);
		else if (!alt && ImGui::IsKeyPressed(ImGuiKey_PageDown))
			MoveDown(GetPageSize() - 4, shift);
		else if (!alt && ctrl && ImGui::IsKeyPressed(ImGuiKey_Home))
			MoveTop(shift);
		else if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_End))
			MoveBottom(shift);
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Home))
			MoveHome(shift);
		else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_End))
			MoveEnd(shift);
		else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
			mDeleted = true;
			if (!HasSelection())
				MoveLeft(1, true, true);
			Backspace();
		}
		else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			mDeleted = true;
			if (!HasSelection())
				MoveRight(1, true, true);
			Delete();
		}
		else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			mDeleted = true;
			Delete();
		}
		else if (!IsReadOnly() && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
			mDeleted = true;
			auto pos = GetActualCursorCoordinates();
			const auto& line = mLines[pos.mLine];
			const int ci = GetCharacterIndex(pos);
			bool done = false;
			if (!HasSelection() && ci > 0 && ci < (int)line.size())
			{
				// An empty pair the editor closed: both go.
				const char a = (char)line[ci - 1].mChar, b = (char)line[ci].mChar;
				if ((a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') ||
				    (a == '"' && b == '"') || (a == '\'' && b == '\''))
				{
					BeginUndoGroup();
					Delete();
					Backspace();
					EndUndoGroup();
					done = true;
				}
			}
			if (!done && !HasSelection() && mInsertSpaces && ci > 0)
			{
				// In the indentation: back to the previous tab stop.
				bool blank = true;
				for (int k = 0; k < ci && blank; ++k)
					blank = line[k].mChar == ' ';
				if (blank && pos.mColumn > 0)
				{
					const int to = ((pos.mColumn - 1) / mTabSize) * mTabSize;
					ReplaceRange(Coordinates(pos.mLine, to), pos, "");
					done = true;
				}
			}
			if (!done)
				Backspace();
		}
		else if (!ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Insert))
			mOverwrite ^= true;
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Insert))
			Copy();
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_C))
			Copy();
		else if (!IsReadOnly() && !ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Insert))
			Paste();
		else if (!IsReadOnly() && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_V))
			Paste();
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_X))
			Cut();
		else if (!ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_Delete))
			Cut();
		else if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_A))
			SelectAll();
		else if (!IsReadOnly() && !ctrl && !shift && !alt &&
		         (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
			TypeCharacter('\n', false);
		else if (!IsReadOnly() && !ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Tab))
		{
			if (shift)
				IndentLines(true);
			else if (HasSelection() && mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine)
				IndentLines(false);
			else
				TypeCharacter('\t', false);
		}

		if (!IsReadOnly() && !io.InputQueueCharacters.empty())
		{
			// Ctrl+key types nothing; AltGr (Ctrl+Alt on Windows) does: { [ ] } \ @ ~ |
			if (!(ctrl && !alt))
				for (int i = 0; i < io.InputQueueCharacters.Size; i++)
				{
					auto c = io.InputQueueCharacters[i];
					if (c != 0 && c != '\n' && c != '\t' && c >= 32)
						TypeCharacter(c, shift);
				}
			io.InputQueueCharacters.resize(0);
		}
	}
}

void CodeEditor::HandleMouseInputs()
{
	ImGuiIO& io = ImGui::GetIO();
	auto shift = io.KeyShift;
	auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

	if (ImGui::IsWindowHovered())
	{
		if (!shift && !alt)
		{
			auto click = ImGui::IsMouseClicked(0);
			auto doubleClick = ImGui::IsMouseDoubleClicked(0);
			auto t = ImGui::GetTime();
			auto tripleClick = click && !doubleClick && (mLastClick != -1.0f && (t - mLastClick) < io.MouseDoubleClickTime);

			/*
			Left mouse button triple click
			*/

			if (tripleClick)
			{
				if (!ctrl)
				{
					mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
					mSelectionMode = SelectionMode::Line;
					SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
				}

				mLastClick = -1.0f;
			}

			/*
			Left mouse button double click
			*/

			else if (doubleClick)
			{
				if (!ctrl)
				{
					mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
					if (mSelectionMode == SelectionMode::Line)
						mSelectionMode = SelectionMode::Normal;
					else
						mSelectionMode = SelectionMode::Word;
					SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
				}

				mLastClick = (float)ImGui::GetTime();
			}

			/*
			Left mouse button click
			*/
			else if (click)
			{
				mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
				if (ctrl)
					mSelectionMode = SelectionMode::Word;
				else
					mSelectionMode = SelectionMode::Normal;
				SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);

				mLastClick = (float)ImGui::GetTime();
			}
			// Mouse left button dragging (=> update selection)
			else if (ImGui::IsMouseDragging(0) && ImGui::IsMouseDown(0))
			{
				io.WantCaptureMouse = true;
				mState.mCursorPosition = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
				SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
			}
		}
	}
}

void CodeEditor::Render()
{
	mFont = ImGui::GetFont();
	mFontSize = ImGui::GetFontSize();
	mFocused = ImGui::IsWindowFocused();
	mViewMin = ImGui::GetWindowPos();
	mViewMax = ImVec2(mViewMin.x + ImGui::GetWindowWidth(), mViewMin.y + ImGui::GetWindowHeight());

	/* Compute mCharAdvance regarding to scaled font size (Ctrl + mouse wheel)*/
	const float fontSize = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, "#", nullptr, nullptr).x;
	mCharAdvance = ImVec2(fontSize, ImGui::GetTextLineHeightWithSpacing() * mLineSpacing);

	/* Update palette with the current alpha from style */
	for (int i = 0; i < (int)PaletteIndex::Max; ++i)
	{
		auto color = ImGui::ColorConvertU32ToFloat4(mPaletteBase[i]);
		color.w *= ImGui::GetStyle().Alpha;
		mPalette[i] = ImGui::ColorConvertFloat4ToU32(color);
	}

	assert(mLineBuffer.empty());

	auto contentSize = ImGui::GetWindowContentRegionMax();
	auto drawList = ImGui::GetWindowDrawList();
	float longest(mTextStart);

	if (mScrollToTop)
	{
		mScrollToTop = false;
		ImGui::SetScrollY(0.f);
	}

	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	mOrigin = cursorScreenPos;
	auto scrollX = ImGui::GetScrollX();
	auto scrollY = ImGui::GetScrollY();

	auto lineNo = (int)floor(scrollY / mCharAdvance.y);
	auto globalLineMax = (int)mLines.size();
	auto lineMax = std::max(0, std::min((int)mLines.size() - 1, lineNo + (int)floor((scrollY + contentSize.y) / mCharAdvance.y)));

	// Deduce mTextStart by evaluating mLines size (global lineMax) plus two spaces as text width
	char buf[16];
	snprintf(buf, 16, " %d ", globalLineMax);
	mTextStart = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x + mLeftMargin;

	// The bracket at the caret and its partner.
	Coordinates bracketOpen, bracketClose;
	const bool bracketMatch = mFocused && !HasSelection() && FindMatchingBracket(GetActualCursorCoordinates(), bracketOpen, bracketClose);

	if (!mLines.empty())
	{
		float spaceSize = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;

		while (lineNo <= lineMax)
		{
			ImVec2 lineStartScreenPos = ImVec2(cursorScreenPos.x, cursorScreenPos.y + lineNo * mCharAdvance.y);
			ImVec2 textScreenPos = ImVec2(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);

			auto& line = mLines[lineNo];
			longest = std::max(mTextStart + TextDistanceToLineStart(Coordinates(lineNo, GetLineMaxColumn(lineNo))), longest);
			auto columnNo = 0;
			Coordinates lineStartCoord(lineNo, 0);
			Coordinates lineEndCoord(lineNo, GetLineMaxColumn(lineNo));

			// Draw selection for the current line
			float sstart = -1.0f;
			float ssend = -1.0f;

			assert(mState.mSelectionStart <= mState.mSelectionEnd);
			if (mState.mSelectionStart <= lineEndCoord)
				sstart = mState.mSelectionStart > lineStartCoord ? TextDistanceToLineStart(mState.mSelectionStart) : 0.0f;
			if (mState.mSelectionEnd > lineStartCoord)
				ssend = TextDistanceToLineStart(mState.mSelectionEnd < lineEndCoord ? mState.mSelectionEnd : lineEndCoord);

			if (mState.mSelectionEnd.mLine > lineNo)
				ssend += mCharAdvance.x;

			if (sstart != -1 && ssend != -1 && sstart < ssend)
			{
				ImVec2 vstart(lineStartScreenPos.x + mTextStart + sstart, lineStartScreenPos.y);
				ImVec2 vend(lineStartScreenPos.x + mTextStart + ssend, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(vstart, vend, mPalette[(int)PaletteIndex::Selection]);
			}

			auto start = ImVec2(lineStartScreenPos.x + scrollX, lineStartScreenPos.y);

			// Highlighted ranges (find matches, references)
			for (auto& h : mHighlights)
			{
				if (lineNo < h.first.mLine || lineNo > h.second.mLine)
					continue;
				float hx0 = lineNo == h.first.mLine ? TextDistanceToLineStart(h.first) : 0.0f;
				float hx1 = lineNo == h.second.mLine ? TextDistanceToLineStart(h.second)
				                                      : TextDistanceToLineStart(lineEndCoord) + mCharAdvance.x;
				if (hx1 <= hx0)
					hx1 = hx0 + 2.0f;
				drawList->AddRectFilled(ImVec2(textScreenPos.x + hx0, lineStartScreenPos.y),
				                        ImVec2(textScreenPos.x + hx1, lineStartScreenPos.y + mCharAdvance.y),
				                        mPalette[(int)PaletteIndex::Highlight]);
			}
			if (bracketMatch)
				for (const Coordinates& b : { bracketOpen, bracketClose })
					if (b.mLine == lineNo)
					{
						const float bx = TextDistanceToLineStart(b);
						drawList->AddRect(ImVec2(textScreenPos.x + bx, lineStartScreenPos.y),
						                  ImVec2(textScreenPos.x + bx + mCharAdvance.x, lineStartScreenPos.y + mCharAdvance.y),
						                  mPalette[(int)PaletteIndex::BracketMatch]);
					}

			// Draw line number (right aligned)
			snprintf(buf, 16, "%d  ", lineNo + 1);

			auto lineNoWidth = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x;
			drawList->AddText(ImVec2(lineStartScreenPos.x + mTextStart - lineNoWidth, lineStartScreenPos.y), mPalette[(int)PaletteIndex::LineNumber], buf);

			if (mState.mCursorPosition.mLine == lineNo)
			{
				auto focused = ImGui::IsWindowFocused();

				// Highlight the current line (where the cursor is)
				if (!HasSelection())
				{
					auto end = ImVec2(start.x + contentSize.x + scrollX, start.y + mCharAdvance.y);
					drawList->AddRectFilled(start, end, mPalette[(int)(focused ? PaletteIndex::CurrentLineFill : PaletteIndex::CurrentLineFillInactive)]);
					drawList->AddRect(start, end, mPalette[(int)PaletteIndex::CurrentLineEdge], 1.0f);
				}

				// Render the cursor
				if (focused)
				{
					auto timeEnd = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
					auto elapsed = timeEnd - mStartTime;
					if (elapsed > 400)
					{
						float width = 1.0f;
						auto cindex = GetCharacterIndex(mState.mCursorPosition);
						float cx = TextDistanceToLineStart(mState.mCursorPosition);

						if (mOverwrite && cindex < (int)line.size())
						{
							auto c = line[cindex].mChar;
							if (c == '\t')
							{
								auto x = (1.0f + std::floor((1.0f + cx) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
								width = x - cx;
							}
							else
							{
								char buf2[2];
								buf2[0] = line[cindex].mChar;
								buf2[1] = '\0';
								width = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, buf2).x;
							}
						}
						ImVec2 cstart(textScreenPos.x + cx, lineStartScreenPos.y);
						ImVec2 cend(textScreenPos.x + cx + width, lineStartScreenPos.y + mCharAdvance.y);
						drawList->AddRectFilled(cstart, cend, mPalette[(int)PaletteIndex::Cursor]);
						if (elapsed > 800)
							mStartTime = timeEnd;
					}
				}
			}

			// Render colorized text
			auto prevColor = line.empty() ? mPalette[(int)PaletteIndex::Default] : GetGlyphColor(line[0]);
			ImVec2 bufferOffset;

			for (int i = 0; i < line.size();)
			{
				auto& glyph = line[i];
				auto color = GetGlyphColor(glyph);

				if ((color != prevColor || glyph.mChar == '\t' || glyph.mChar == ' ') && !mLineBuffer.empty())
				{
					const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
					drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
					auto textSize = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, mLineBuffer.c_str(), nullptr, nullptr);
					bufferOffset.x += textSize.x;
					mLineBuffer.clear();
				}
				prevColor = color;

				if (glyph.mChar == '\t')
				{
					auto oldX = bufferOffset.x;
					bufferOffset.x = (1.0f + std::floor((1.0f + bufferOffset.x) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
					++i;

					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x1 = textScreenPos.x + oldX + 1.0f;
						const auto x2 = textScreenPos.x + bufferOffset.x - 1.0f;
						const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
						const ImVec2 p1(x1, y);
						const ImVec2 p2(x2, y);
						const ImVec2 p3(x2 - s * 0.2f, y - s * 0.2f);
						const ImVec2 p4(x2 - s * 0.2f, y + s * 0.2f);
						drawList->AddLine(p1, p2, 0x90909090);
						drawList->AddLine(p2, p3, 0x90909090);
						drawList->AddLine(p2, p4, 0x90909090);
					}
				}
				else if (glyph.mChar == ' ')
				{
					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x = textScreenPos.x + bufferOffset.x + spaceSize * 0.5f;
						const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
						drawList->AddCircleFilled(ImVec2(x, y), 1.5f, 0x80808080, 4);
					}
					bufferOffset.x += spaceSize;
					i++;
				}
				else
				{
					auto l = UTF8CharLength(glyph.mChar);
					while (l-- > 0)
						mLineBuffer.push_back(line[i++].mChar);
				}
				++columnNo;
			}

			if (!mLineBuffer.empty())
			{
				const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
				drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
				mLineBuffer.clear();
			}

			// Squiggles under the glyphs the language service marked
			for (int gi = 0; gi < (int)line.size();)
			{
				const int sev = line[gi].mDiag;
				if (sev == 0)
				{
					++gi;
					continue;
				}
				int gj = gi;
				while (gj < (int)line.size() && line[gj].mDiag == sev)
					++gj;
				const float sx0 = textScreenPos.x + TextDistanceToLineStart(Coordinates(lineNo, GetCharacterColumn(lineNo, gi)));
				float sx1 = textScreenPos.x + TextDistanceToLineStart(Coordinates(lineNo, GetCharacterColumn(lineNo, gj)));
				if (sx1 - sx0 < mCharAdvance.x * 0.5f)
					sx1 = sx0 + mCharAdvance.x;
				const ImU32 scol = mPalette[(int)(sev >= 3 ? PaletteIndex::ErrorSquiggle
				                                  : sev == 2 ? PaletteIndex::WarningSquiggle : PaletteIndex::InfoSquiggle)];
				const float sy = lineStartScreenPos.y + mCharAdvance.y - 2.0f;
				const float step = 2.0f;
				bool up = true;
				ImVec2 sp(sx0, sy);
				for (float x = sx0 + step; x <= sx1 + 0.01f; x += step)
				{
					ImVec2 np(x, up ? sy - step : sy);
					drawList->AddLine(sp, np, scol, 1.0f);
					sp = np;
					up = !up;
				}
				gi = gj;
			}

			++lineNo;
		}

	}


	ImGui::Dummy(ImVec2((longest + 2), mLines.size() * mCharAdvance.y));

	if (mScrollToCursor)
	{
		EnsureCursorVisible();
		if (mStealFocus)
			ImGui::SetWindowFocus();
		mStealFocus = true;
		mScrollToCursor = false;
	}
}

void CodeEditor::Render(const char* aTitle, const ImVec2& aSize, bool aBorder)
{
	mWithinRender = true;
	mTextChanged = false;
	mCursorPositionChanged = false;
	mTyped.clear();
	mDeleted = false;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
	if (!mIgnoreImGuiChild)
		ImGui::BeginChild(aTitle, aSize, aBorder, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoMove);

	if (mHandleKeyboardInputs)
	{
		HandleKeyboardInputs();
		ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, false);
	}

	if (mHandleMouseInputs)
		HandleMouseInputs();

	ColorizeInternal();
	Render();

	if (mHandleKeyboardInputs)
		ImGui::PopItemFlag();

	if (!mIgnoreImGuiChild)
		ImGui::EndChild();

	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	mWithinRender = false;
}

void CodeEditor::SetText(const std::string & aText)
{
	mLines.clear();
	mLines.emplace_back(Line());
	for (auto chr : aText)
	{
		if (chr == '\r')
		{
			// ignore the carriage return character
		}
		else if (chr == '\n')
			mLines.emplace_back(Line());
		else
		{
			mLines.back().emplace_back(Glyph(chr, PaletteIndex::Default));
		}
	}

	mTextChanged = true;
	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
}

void CodeEditor::SetTextLines(const std::vector<std::string> & aLines)
{
	mLines.clear();

	if (aLines.empty())
	{
		mLines.emplace_back(Line());
	}
	else
	{
		mLines.resize(aLines.size());

		for (size_t i = 0; i < aLines.size(); ++i)
		{
			const std::string & aLine = aLines[i];

			mLines[i].reserve(aLine.size());
			for (size_t j = 0; j < aLine.size(); ++j)
				mLines[i].emplace_back(Glyph(aLine[j], PaletteIndex::Default));
		}
	}

	mTextChanged = true;
	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
}

void CodeEditor::EnterCharacter(ImWchar aChar, bool aShift)
{
	assert(!mReadOnly);

	UndoRecord u;

	u.mBefore = mState;

	if (HasSelection())
	{
		if (aChar == '\t' && mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine)
		{

			auto start = mState.mSelectionStart;
			auto end = mState.mSelectionEnd;
			auto originalEnd = end;

			if (start > end)
				std::swap(start, end);
			start.mColumn = 0;
			//			end.mColumn = end.mLine < mLines.size() ? mLines[end.mLine].size() : 0;
			if (end.mColumn == 0 && end.mLine > 0)
				--end.mLine;
			if (end.mLine >= (int)mLines.size())
				end.mLine = mLines.empty() ? 0 : (int)mLines.size() - 1;
			end.mColumn = GetLineMaxColumn(end.mLine);

			//if (end.mColumn >= GetLineMaxColumn(end.mLine))
			//	end.mColumn = GetLineMaxColumn(end.mLine) - 1;

			u.mRemovedStart = start;
			u.mRemovedEnd = end;
			u.mRemoved = GetText(start, end);

			bool modified = false;

			for (int i = start.mLine; i <= end.mLine; i++)
			{
				auto& line = mLines[i];
				if (aShift)
				{
					if (!line.empty())
					{
						if (line.front().mChar == '\t')
						{
							line.erase(line.begin());
							modified = true;
						}
						else
						{
							for (int j = 0; j < mTabSize && !line.empty() && line.front().mChar == ' '; j++)
							{
								line.erase(line.begin());
								modified = true;
							}
						}
					}
				}
				else
				{
					line.insert(line.begin(), Glyph('\t', CodeEditor::PaletteIndex::Background));
					modified = true;
				}
			}

			if (modified)
			{
				start = Coordinates(start.mLine, GetCharacterColumn(start.mLine, 0));
				Coordinates rangeEnd;
				if (originalEnd.mColumn != 0)
				{
					end = Coordinates(end.mLine, GetLineMaxColumn(end.mLine));
					rangeEnd = end;
					u.mAdded = GetText(start, end);
				}
				else
				{
					end = Coordinates(originalEnd.mLine, 0);
					rangeEnd = Coordinates(end.mLine - 1, GetLineMaxColumn(end.mLine - 1));
					u.mAdded = GetText(start, rangeEnd);
				}

				u.mAddedStart = start;
				u.mAddedEnd = rangeEnd;
				u.mAfter = mState;

				mState.mSelectionStart = start;
				mState.mSelectionEnd = end;
				AddUndo(u);

				mTextChanged = true;

				EnsureCursorVisible();
			}

			return;
		} // c == '\t'
		else
		{
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;
			DeleteSelection();
		}
	} // HasSelection

	auto coord = GetActualCursorCoordinates();
	u.mAddedStart = coord;

	assert(!mLines.empty());

	if (aChar == '\n')
	{
		InsertLine(coord.mLine + 1);
		auto& line = mLines[coord.mLine];
		auto& newLine = mLines[coord.mLine + 1];

		if (mLanguageDefinition.mAutoIndentation)
			for (size_t it = 0; it < line.size() && isascii(line[it].mChar) && isblank(line[it].mChar); ++it)
				newLine.push_back(line[it]);

		const size_t whitespaceSize = newLine.size();
		auto cindex = GetCharacterIndex(coord);
		newLine.insert(newLine.end(), line.begin() + cindex, line.end());
		line.erase(line.begin() + cindex, line.begin() + line.size());
		SetCursorPosition(Coordinates(coord.mLine + 1, GetCharacterColumn(coord.mLine + 1, (int)whitespaceSize)));
		u.mAdded = (char)aChar;
	}
	else
	{
		char buf[7];
		int e = ImTextCharToUtf8(buf, 7, aChar);
		if (e > 0)
		{
			buf[e] = '\0';
			auto& line = mLines[coord.mLine];
			auto cindex = GetCharacterIndex(coord);

			if (mOverwrite && cindex < (int)line.size())
			{
				auto d = UTF8CharLength(line[cindex].mChar);

				u.mRemovedStart = mState.mCursorPosition;
				u.mRemovedEnd = Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex + d));

				while (d-- > 0 && cindex < (int)line.size())
				{
					u.mRemoved += line[cindex].mChar;
					line.erase(line.begin() + cindex);
				}
			}

			for (auto p = buf; *p != '\0'; p++, ++cindex)
				line.insert(line.begin() + cindex, Glyph(*p, PaletteIndex::Default));
			u.mAdded = buf;

			SetCursorPosition(Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex)));
		}
		else
			return;
	}

	mTextChanged = true;

	u.mAddedEnd = GetActualCursorCoordinates();
	u.mAfter = mState;

	AddUndo(u);

	Colorize(coord.mLine - 1, 3);
	EnsureCursorVisible();
}

void CodeEditor::SetReadOnly(bool aValue)
{
	mReadOnly = aValue;
}

void CodeEditor::SetColorizerEnable(bool aValue)
{
	mColorizerEnabled = aValue;
}

void CodeEditor::SetCursorPosition(const Coordinates & aPosition)
{
	if (mState.mCursorPosition != aPosition)
	{
		mState.mCursorPosition = aPosition;
		mCursorPositionChanged = true;
		EnsureCursorVisible();
	}
}

void CodeEditor::SetSelectionStart(const Coordinates & aPosition)
{
	mState.mSelectionStart = SanitizeCoordinates(aPosition);
	if (mState.mSelectionStart > mState.mSelectionEnd)
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void CodeEditor::SetSelectionEnd(const Coordinates & aPosition)
{
	mState.mSelectionEnd = SanitizeCoordinates(aPosition);
	if (mState.mSelectionStart > mState.mSelectionEnd)
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
}

void CodeEditor::SetSelection(const Coordinates & aStart, const Coordinates & aEnd, SelectionMode aMode)
{
	auto oldSelStart = mState.mSelectionStart;
	auto oldSelEnd = mState.mSelectionEnd;

	mState.mSelectionStart = SanitizeCoordinates(aStart);
	mState.mSelectionEnd = SanitizeCoordinates(aEnd);
	if (mState.mSelectionStart > mState.mSelectionEnd)
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);

	switch (aMode)
	{
	case CodeEditor::SelectionMode::Normal:
		break;
	case CodeEditor::SelectionMode::Word:
	{
		mState.mSelectionStart = FindWordStart(mState.mSelectionStart);
		if (!IsOnWordBoundary(mState.mSelectionEnd))
			mState.mSelectionEnd = FindWordEnd(FindWordStart(mState.mSelectionEnd));
		break;
	}
	case CodeEditor::SelectionMode::Line:
	{
		const auto lineNo = mState.mSelectionEnd.mLine;
		const auto lineSize = (size_t)lineNo < mLines.size() ? mLines[lineNo].size() : 0;
		mState.mSelectionStart = Coordinates(mState.mSelectionStart.mLine, 0);
		mState.mSelectionEnd = Coordinates(lineNo, GetLineMaxColumn(lineNo));
		break;
	}
	default:
		break;
	}

	if (mState.mSelectionStart != oldSelStart ||
		mState.mSelectionEnd != oldSelEnd)
		mCursorPositionChanged = true;
}

void CodeEditor::SetTabSize(int aValue)
{
	mTabSize = std::max(0, std::min(32, aValue));
}

void CodeEditor::InsertText(const std::string & aValue)
{
	InsertText(aValue.c_str());
}

void CodeEditor::InsertText(const char * aValue)
{
	if (aValue == nullptr)
		return;

	auto pos = GetActualCursorCoordinates();
	auto start = std::min(pos, mState.mSelectionStart);
	int totalLines = pos.mLine - start.mLine;

	totalLines += InsertTextAt(pos, aValue);

	SetSelection(pos, pos);
	SetCursorPosition(pos);
	Colorize(start.mLine - 1, totalLines + 2);
}

void CodeEditor::DeleteSelection()
{
	assert(mState.mSelectionEnd >= mState.mSelectionStart);

	if (mState.mSelectionEnd == mState.mSelectionStart)
		return;

	DeleteRange(mState.mSelectionStart, mState.mSelectionEnd);

	SetSelection(mState.mSelectionStart, mState.mSelectionStart);
	SetCursorPosition(mState.mSelectionStart);
	Colorize(mState.mSelectionStart.mLine, 1);
}

void CodeEditor::MoveUp(int aAmount, bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition.mLine = std::max(0, mState.mCursorPosition.mLine - aAmount);
	if (oldPos != mState.mCursorPosition)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else
			{
				mInteractiveStart = mState.mCursorPosition;
				mInteractiveEnd = oldPos;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);

		EnsureCursorVisible();
	}
}

void CodeEditor::MoveDown(int aAmount, bool aSelect)
{
	assert(mState.mCursorPosition.mColumn >= 0);
	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + aAmount));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else
			{
				mInteractiveStart = oldPos;
				mInteractiveEnd = mState.mCursorPosition;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);

		EnsureCursorVisible();
	}
}

static bool IsUTFSequence(char c)
{
	return (c & 0xC0) == 0x80;
}

void CodeEditor::MoveLeft(int aAmount, bool aSelect, bool aWordMode)
{
	if (mLines.empty())
		return;

	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition = GetActualCursorCoordinates();
	auto line = mState.mCursorPosition.mLine;
	auto cindex = GetCharacterIndex(mState.mCursorPosition);

	while (aAmount-- > 0)
	{
		if (cindex == 0)
		{
			if (line > 0)
			{
				--line;
				if ((int)mLines.size() > line)
					cindex = (int)mLines[line].size();
				else
					cindex = 0;
			}
		}
		else
		{
			--cindex;
			if (cindex > 0)
			{
				if ((int)mLines.size() > line)
				{
					while (cindex > 0 && IsUTFSequence(mLines[line][cindex].mChar))
						--cindex;
				}
			}
		}

		mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));
		if (aWordMode)
		{
			mState.mCursorPosition = FindWordStart(mState.mCursorPosition);
			cindex = GetCharacterIndex(mState.mCursorPosition);
		}
	}

	mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));

	assert(mState.mCursorPosition.mColumn >= 0);
	if (aSelect)
	{
		if (oldPos == mInteractiveStart)
			mInteractiveStart = mState.mCursorPosition;
		else if (oldPos == mInteractiveEnd)
			mInteractiveEnd = mState.mCursorPosition;
		else
		{
			mInteractiveStart = mState.mCursorPosition;
			mInteractiveEnd = oldPos;
		}
	}
	else
		mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
	SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

	EnsureCursorVisible();
}

void CodeEditor::MoveRight(int aAmount, bool aSelect, bool aWordMode)
{
	auto oldPos = mState.mCursorPosition;

	if (mLines.empty() || oldPos.mLine >= mLines.size())
		return;

	auto cindex = GetCharacterIndex(mState.mCursorPosition);
	while (aAmount-- > 0)
	{
		auto lindex = mState.mCursorPosition.mLine;
		auto& line = mLines[lindex];

		if (cindex >= line.size())
		{
			if (mState.mCursorPosition.mLine < mLines.size() - 1)
			{
				mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + 1));
				mState.mCursorPosition.mColumn = 0;
			}
			else
				return;
		}
		else
		{
			cindex += UTF8CharLength(line[cindex].mChar);
			mState.mCursorPosition = Coordinates(lindex, GetCharacterColumn(lindex, cindex));
			if (aWordMode)
				mState.mCursorPosition = FindNextWord(mState.mCursorPosition);
		}
	}

	if (aSelect)
	{
		if (oldPos == mInteractiveEnd)
			mInteractiveEnd = SanitizeCoordinates(mState.mCursorPosition);
		else if (oldPos == mInteractiveStart)
			mInteractiveStart = mState.mCursorPosition;
		else
		{
			mInteractiveStart = oldPos;
			mInteractiveEnd = mState.mCursorPosition;
		}
	}
	else
		mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
	SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

	EnsureCursorVisible();
}

void CodeEditor::MoveTop(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(0, 0));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			mInteractiveEnd = oldPos;
			mInteractiveStart = mState.mCursorPosition;
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void CodeEditor::CodeEditor::MoveBottom(bool aSelect)
{
	auto oldPos = GetCursorPosition();
	auto newPos = Coordinates((int)mLines.size() - 1, 0);
	SetCursorPosition(newPos);
	if (aSelect)
	{
		mInteractiveStart = oldPos;
		mInteractiveEnd = newPos;
	}
	else
		mInteractiveStart = mInteractiveEnd = newPos;
	SetSelection(mInteractiveStart, mInteractiveEnd);
}

void CodeEditor::MoveHome(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	{
		const auto& hl = mLines[mState.mCursorPosition.mLine];
		int first = 0;
		while (first < (int)hl.size() && (hl[first].mChar == ' ' || hl[first].mChar == '\t'))
			++first;
		const int firstCol = GetCharacterColumn(mState.mCursorPosition.mLine, first);
		SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, oldPos.mColumn == firstCol ? 0 : firstCol));
	}

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else
			{
				mInteractiveStart = mState.mCursorPosition;
				mInteractiveEnd = oldPos;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void CodeEditor::MoveEnd(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, GetLineMaxColumn(oldPos.mLine)));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveEnd)
				mInteractiveEnd = mState.mCursorPosition;
			else if (oldPos == mInteractiveStart)
				mInteractiveStart = mState.mCursorPosition;
			else
			{
				mInteractiveStart = oldPos;
				mInteractiveEnd = mState.mCursorPosition;
			}
		}
		else
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void CodeEditor::Delete()
{
	assert(!mReadOnly);

	if (mLines.empty())
		return;

	UndoRecord u;
	u.mBefore = mState;

	if (HasSelection())
	{
		u.mRemoved = GetSelectedText();
		u.mRemovedStart = mState.mSelectionStart;
		u.mRemovedEnd = mState.mSelectionEnd;

		DeleteSelection();
	}
	else
	{
		auto pos = GetActualCursorCoordinates();
		SetCursorPosition(pos);
		auto& line = mLines[pos.mLine];

		if (pos.mColumn == GetLineMaxColumn(pos.mLine))
		{
			if (pos.mLine == (int)mLines.size() - 1)
				return;

			u.mRemoved = '\n';
			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			Advance(u.mRemovedEnd);

			auto& nextLine = mLines[pos.mLine + 1];
			line.insert(line.end(), nextLine.begin(), nextLine.end());
			RemoveLine(pos.mLine + 1);
		}
		else
		{
			auto cindex = GetCharacterIndex(pos);
			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			u.mRemovedEnd.mColumn++;
			u.mRemoved = GetText(u.mRemovedStart, u.mRemovedEnd);

			auto d = UTF8CharLength(line[cindex].mChar);
			while (d-- > 0 && cindex < (int)line.size())
				line.erase(line.begin() + cindex);
		}

		mTextChanged = true;

		Colorize(pos.mLine, 1);
	}

	u.mAfter = mState;
	AddUndo(u);
}

void CodeEditor::Backspace()
{
	assert(!mReadOnly);

	if (mLines.empty())
		return;

	UndoRecord u;
	u.mBefore = mState;

	if (HasSelection())
	{
		u.mRemoved = GetSelectedText();
		u.mRemovedStart = mState.mSelectionStart;
		u.mRemovedEnd = mState.mSelectionEnd;

		DeleteSelection();
	}
	else
	{
		auto pos = GetActualCursorCoordinates();
		SetCursorPosition(pos);

		if (mState.mCursorPosition.mColumn == 0)
		{
			if (mState.mCursorPosition.mLine == 0)
				return;

			u.mRemoved = '\n';
			u.mRemovedStart = u.mRemovedEnd = Coordinates(pos.mLine - 1, GetLineMaxColumn(pos.mLine - 1));
			Advance(u.mRemovedEnd);

			auto& line = mLines[mState.mCursorPosition.mLine];
			auto& prevLine = mLines[mState.mCursorPosition.mLine - 1];
			auto prevSize = GetLineMaxColumn(mState.mCursorPosition.mLine - 1);
			prevLine.insert(prevLine.end(), line.begin(), line.end());

			RemoveLine(mState.mCursorPosition.mLine);
			--mState.mCursorPosition.mLine;
			mState.mCursorPosition.mColumn = prevSize;
		}
		else
		{
			auto& line = mLines[mState.mCursorPosition.mLine];
			auto cindex = GetCharacterIndex(pos) - 1;
			auto cend = cindex + 1;
			while (cindex > 0 && IsUTFSequence(line[cindex].mChar))
				--cindex;

			//if (cindex > 0 && UTF8CharLength(line[cindex].mChar) > 1)
			//	--cindex;

			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			--u.mRemovedStart.mColumn;
			--mState.mCursorPosition.mColumn;

			while (cindex < line.size() && cend-- > cindex)
			{
				u.mRemoved += line[cindex].mChar;
				line.erase(line.begin() + cindex);
			}
		}

		mTextChanged = true;

		EnsureCursorVisible();
		Colorize(mState.mCursorPosition.mLine, 1);
	}

	u.mAfter = mState;
	AddUndo(u);
}

void CodeEditor::SelectWordUnderCursor()
{
	auto c = GetCursorPosition();
	SetSelection(FindWordStart(c), FindWordEnd(c));
}

void CodeEditor::SelectAll()
{
	SetSelection(Coordinates(0, 0), Coordinates((int)mLines.size(), 0));
}

bool CodeEditor::HasSelection() const
{
	return mState.mSelectionEnd > mState.mSelectionStart;
}

void CodeEditor::Copy()
{
	if (HasSelection())
	{
		ImGui::SetClipboardText(GetSelectedText().c_str());
	}
	else
	{
		if (!mLines.empty())
		{
			std::string str;
			auto& line = mLines[GetActualCursorCoordinates().mLine];
			for (auto& g : line)
				str.push_back(g.mChar);
			ImGui::SetClipboardText(str.c_str());
		}
	}
}

void CodeEditor::Cut()
{
	if (IsReadOnly())
	{
		Copy();
	}
	else
	{
		if (HasSelection())
		{
			UndoRecord u;
			u.mBefore = mState;
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;

			Copy();
			DeleteSelection();

			u.mAfter = mState;
			AddUndo(u);
		}
	}
}

void CodeEditor::Paste()
{
	if (IsReadOnly())
		return;

	auto clipText = ImGui::GetClipboardText();
	if (clipText != nullptr && strlen(clipText) > 0)
	{
		UndoRecord u;
		u.mBefore = mState;

		if (HasSelection())
		{
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;
			DeleteSelection();
		}

		u.mAdded = clipText;
		u.mAddedStart = GetActualCursorCoordinates();

		InsertText(clipText);

		u.mAddedEnd = GetActualCursorCoordinates();
		u.mAfter = mState;
		AddUndo(u);
	}
}

bool CodeEditor::CanUndo() const
{
	return !mReadOnly && mUndoIndex > 0;
}

bool CodeEditor::CanRedo() const
{
	return !mReadOnly && mUndoIndex < (int)mUndoBuffer.size();
}

void CodeEditor::Undo(int aSteps)
{
	while (CanUndo() && aSteps-- > 0)
	{
		const int group = mUndoBuffer[mUndoIndex - 1].mGroup;
		do
			mUndoBuffer[--mUndoIndex].Undo(this);
		while (group != 0 && mUndoIndex > 0 && mUndoBuffer[mUndoIndex - 1].mGroup == group);
	}
}

void CodeEditor::Redo(int aSteps)
{
	while (CanRedo() && aSteps-- > 0)
	{
		const int group = mUndoBuffer[mUndoIndex].mGroup;
		do
			mUndoBuffer[mUndoIndex++].Redo(this);
		while (group != 0 && mUndoIndex < (int)mUndoBuffer.size() && mUndoBuffer[mUndoIndex].mGroup == group);
	}
}

const CodeEditor::Palette & CodeEditor::GetDarkPalette()
{
	// ABGR. Visual Studio Code's Dark+ colours.
	const static Palette p = { {
			0xffd4d4d4,	// Default
			0xffd69c56,	// Keyword
			0xffa8ceb5,	// Number
			0xff7891ce,	// String
			0xff7891ce, // Char literal
			0xffd4d4d4, // Punctuation
			0xff9b9b9b,	// Preprocessor
			0xffd4d4d4, // Identifier
			0xffb0c94e, // Known identifier
			0xffc086c5, // Preproc identifier
			0xff55996a, // Comment (single line)
			0xff55996a, // Comment (multi line)
			0xff1e1e1e, // Background
			0xffadafae, // Cursor
			0xff784f26, // Selection
			0x800020ff, // ErrorMarker
			0x40f08000, // Breakpoint
			0xff858585, // Line number
			0x10ffffff, // Current line fill
			0x08ffffff, // Current line fill (inactive)
			0x18ffffff, // Current line edge
			0xffb0c94e, // Type
			0xffaadcdc, // Method
			0xfffedc9c, // Member
			0xfffedc9c, // Variable
			0xffffc14f, // Enum member / constant
			0xffb0c94e, // Namespace
			0xffc086c5, // Control keyword
			0xff808080, // Excluded code
			0xff7dbad7, // String escape
			0xff4c4cf1, // Error squiggle
			0xff00a7cc, // Warning squiggle
			0xffff9437, // Info squiggle
			0x806a5c51, // Highlight
			0xff909090, // Bracket match
		} };
	return p;
}


std::string CodeEditor::GetText() const
{
	return GetText(Coordinates(), Coordinates((int)mLines.size(), 0));
}

std::vector<std::string> CodeEditor::GetTextLines() const
{
	std::vector<std::string> result;

	result.reserve(mLines.size());

	for (auto & line : mLines)
	{
		std::string text;

		text.resize(line.size());

		for (size_t i = 0; i < line.size(); ++i)
			text[i] = line[i].mChar;

		result.emplace_back(std::move(text));
	}

	return result;
}

std::string CodeEditor::GetSelectedText() const
{
	return GetText(mState.mSelectionStart, mState.mSelectionEnd);
}

std::string CodeEditor::GetCurrentLineText()const
{
	auto lineLength = GetLineMaxColumn(mState.mCursorPosition.mLine);
	return GetText(
		Coordinates(mState.mCursorPosition.mLine, 0),
		Coordinates(mState.mCursorPosition.mLine, lineLength));
}

void CodeEditor::Colorize(int aFromLine, int aLines)
{
	int toLine = aLines == -1 ? (int)mLines.size() : std::min((int)mLines.size(), aFromLine + aLines);
	mColorRangeMin = std::min(mColorRangeMin, aFromLine);
	mColorRangeMax = std::max(mColorRangeMax, toLine);
	mColorRangeMin = std::max(0, mColorRangeMin);
	mColorRangeMax = std::max(mColorRangeMin, mColorRangeMax);
	mCheckComments = true;
}

void CodeEditor::ColorizeRange(int aFromLine, int aToLine)
{
	if (mLines.empty() || aFromLine >= aToLine)
		return;

	std::string buffer;
	std::string id;

	int endLine = std::max(0, std::min((int)mLines.size(), aToLine));
	for (int i = aFromLine; i < endLine; ++i)
	{
		auto& line = mLines[i];

		if (line.empty())
			continue;

		buffer.resize(line.size());
		for (size_t j = 0; j < line.size(); ++j)
		{
			auto& col = line[j];
			buffer[j] = col.mChar;
			col.mColorIndex = PaletteIndex::Default;
		}

		const char * bufferBegin = &buffer.front();
		const char * bufferEnd = bufferBegin + buffer.size();

		auto last = bufferEnd;

		for (auto first = bufferBegin; first != last; )
		{
			const char * token_begin = nullptr;
			const char * token_end = nullptr;
			PaletteIndex token_color = PaletteIndex::Default;

			bool hasTokenizeResult = false;

			if (mLanguageDefinition.mTokenize != nullptr)
			{
				if (mLanguageDefinition.mTokenize(first, last, token_begin, token_end, token_color))
					hasTokenizeResult = true;
			}

			if (hasTokenizeResult == false)
			{
				first++;
			}
			else
			{
				const size_t token_length = token_end - token_begin;

				if (token_color == PaletteIndex::Identifier)
				{
					id.assign(token_begin, token_end);

					// todo : allmost all language definitions use lower case to specify keywords, so shouldn't this use ::tolower ?
					if (!mLanguageDefinition.mCaseSensitive)
						std::transform(id.begin(), id.end(), id.begin(), ::toupper);

					if (!line[first - bufferBegin].mPreprocessor)
					{
						if (mLanguageDefinition.mControlKeywords.count(id) != 0)
							token_color = PaletteIndex::ControlKeyword;
						else if (mLanguageDefinition.mKeywords.count(id) != 0)
							token_color = PaletteIndex::Keyword;
						else if (mLanguageDefinition.mIdentifiers.count(id) != 0)
							token_color = PaletteIndex::KnownIdentifier;
						else if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
							token_color = PaletteIndex::PreprocIdentifier;
					}
					else
					{
						if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
							token_color = PaletteIndex::PreprocIdentifier;
					}
				}

				for (size_t j = 0; j < token_length; ++j)
					line[(token_begin - bufferBegin) + j].mColorIndex = token_color;

				first = token_end;
			}
		}
	}
}

void CodeEditor::ColorizeInternal()
{
	if (mLines.empty() || !mColorizerEnabled)
		return;

	if (mCheckComments)
	{
		auto endLine = mLines.size();
		auto endIndex = 0;
		auto commentStartLine = endLine;
		auto commentStartIndex = endIndex;
		auto withinString = false;
		auto withinSingleLineComment = false;
		auto withinPreproc = false;
		auto firstChar = true;			// there is no other non-whitespace characters in the line before
		auto concatenate = false;		// '\' on the very end of the line
		auto currentLine = 0;
		auto currentIndex = 0;
		while (currentLine < endLine || currentIndex < endIndex)
		{
			auto& line = mLines[currentLine];

			if (currentIndex == 0 && !concatenate)
			{
				withinSingleLineComment = false;
				withinPreproc = false;
				firstChar = true;
			}

			concatenate = false;

			if (!line.empty())
			{
				auto& g = line[currentIndex];
				auto c = g.mChar;

				if (c != mLanguageDefinition.mPreprocChar && !isspace(c))
					firstChar = false;

				if (currentIndex == (int)line.size() - 1 && line[line.size() - 1].mChar == '\\')
					concatenate = true;

				bool inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

				if (withinString)
				{
					line[currentIndex].mMultiLineComment = inComment;

					if (c == '\"')
					{
						if (currentIndex + 1 < (int)line.size() && line[currentIndex + 1].mChar == '\"')
						{
							currentIndex += 1;
							if (currentIndex < (int)line.size())
								line[currentIndex].mMultiLineComment = inComment;
						}
						else
							withinString = false;
					}
					else if (c == '\\')
					{
						currentIndex += 1;
						if (currentIndex < (int)line.size())
							line[currentIndex].mMultiLineComment = inComment;
					}
				}
				else
				{
					if (firstChar && c == mLanguageDefinition.mPreprocChar)
						withinPreproc = true;

					if (c == '\"')
					{
						withinString = true;
						line[currentIndex].mMultiLineComment = inComment;
					}
					else
					{
						auto pred = [](const char& a, const Glyph& b) { return a == b.mChar; };
						auto from = line.begin() + currentIndex;
						auto& startStr = mLanguageDefinition.mCommentStart;
						auto& singleStartStr = mLanguageDefinition.mSingleLineComment;

						if (singleStartStr.size() > 0 &&
							currentIndex + singleStartStr.size() <= line.size() &&
							equals(singleStartStr.begin(), singleStartStr.end(), from, from + singleStartStr.size(), pred))
						{
							withinSingleLineComment = true;
						}
						else if (!withinSingleLineComment && currentIndex + startStr.size() <= line.size() &&
							equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
						{
							commentStartLine = currentLine;
							commentStartIndex = currentIndex;
						}

						inComment = inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

						line[currentIndex].mMultiLineComment = inComment;
						line[currentIndex].mComment = withinSingleLineComment;

						auto& endStr = mLanguageDefinition.mCommentEnd;
						if (currentIndex + 1 >= (int)endStr.size() &&
							equals(endStr.begin(), endStr.end(), from + 1 - endStr.size(), from + 1, pred))
						{
							commentStartIndex = endIndex;
							commentStartLine = endLine;
						}
					}
				}
				line[currentIndex].mPreprocessor = withinPreproc;
				currentIndex += UTF8CharLength(c);
				if (currentIndex >= (int)line.size())
				{
					currentIndex = 0;
					++currentLine;
				}
			}
			else
			{
				currentIndex = 0;
				++currentLine;
			}
		}
		mCheckComments = false;
	}

	if (mColorRangeMin < mColorRangeMax)
	{
		const int increment = (mLanguageDefinition.mTokenize == nullptr) ? 10 : 10000;
		const int to = std::min(mColorRangeMin + increment, mColorRangeMax);
		ColorizeRange(mColorRangeMin, to);
		mColorRangeMin = to;

		if (mColorRangeMax == mColorRangeMin)
		{
			mColorRangeMin = std::numeric_limits<int>::max();
			mColorRangeMax = 0;
		}
		return;
	}
}

float CodeEditor::TextDistanceToLineStart(const Coordinates& aFrom) const
{
	auto& line = mLines[aFrom.mLine];
	float distance = 0.0f;
	float spaceSize = Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;
	int colIndex = GetCharacterIndex(aFrom);
	for (size_t it = 0u; it < line.size() && it < colIndex; )
	{
		if (line[it].mChar == '\t')
		{
			distance = (1.0f + std::floor((1.0f + distance) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
			++it;
		}
		else
		{
			auto d = UTF8CharLength(line[it].mChar);
			char tempCString[7];
			int i = 0;
			for (; i < 6 && d-- > 0 && it < (int)line.size(); i++, it++)
				tempCString[i] = line[it].mChar;

			tempCString[i] = '\0';
			distance += Font()->CalcTextSizeA(FontSize(), FLT_MAX, -1.0f, tempCString, nullptr, nullptr).x;
		}
	}

	return distance;
}

void CodeEditor::EnsureCursorVisible()
{
	if (!mWithinRender)
	{
		mScrollToCursor = true;
		return;
	}

	float scrollX = ImGui::GetScrollX();
	float scrollY = ImGui::GetScrollY();

	auto height = ImGui::GetWindowHeight();
	auto width = ImGui::GetWindowWidth();

	auto top = 1 + (int)ceil(scrollY / mCharAdvance.y);
	auto bottom = (int)ceil((scrollY + height) / mCharAdvance.y);

	auto left = (int)ceil(scrollX / mCharAdvance.x);
	auto right = (int)ceil((scrollX + width) / mCharAdvance.x);

	auto pos = GetActualCursorCoordinates();
	auto len = TextDistanceToLineStart(pos);

	if (mCenterOnCursor)
	{
		mCenterOnCursor = false;
		ImGui::SetScrollY(std::max(0.0f, pos.mLine * mCharAdvance.y - height * 0.35f));
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart - width * 0.5f));
		return;
	}

	if (pos.mLine < top)
		ImGui::SetScrollY(std::max(0.0f, (pos.mLine - 1) * mCharAdvance.y));
	if (pos.mLine > bottom - 4)
		ImGui::SetScrollY(std::max(0.0f, (pos.mLine + 4) * mCharAdvance.y - height));
	if (len + mTextStart < left + 4)
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart - 4));
	if (len + mTextStart > right - 4)
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart + 4 - width));
}

int CodeEditor::GetPageSize() const
{
	auto height = ImGui::GetWindowHeight() - 20.0f;
	return (int)floor(height / mCharAdvance.y);
}

CodeEditor::UndoRecord::UndoRecord(
	const std::string& aAdded,
	const CodeEditor::Coordinates aAddedStart,
	const CodeEditor::Coordinates aAddedEnd,
	const std::string& aRemoved,
	const CodeEditor::Coordinates aRemovedStart,
	const CodeEditor::Coordinates aRemovedEnd,
	CodeEditor::EditorState& aBefore,
	CodeEditor::EditorState& aAfter)
	: mAdded(aAdded)
	, mAddedStart(aAddedStart)
	, mAddedEnd(aAddedEnd)
	, mRemoved(aRemoved)
	, mRemovedStart(aRemovedStart)
	, mRemovedEnd(aRemovedEnd)
	, mBefore(aBefore)
	, mAfter(aAfter)
{
	assert(mAddedStart <= mAddedEnd);
	assert(mRemovedStart <= mRemovedEnd);
}

void CodeEditor::UndoRecord::Undo(CodeEditor * aEditor)
{
	if (!mAdded.empty())
	{
		aEditor->DeleteRange(mAddedStart, mAddedEnd);
		aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 2);
	}

	if (!mRemoved.empty())
	{
		auto start = mRemovedStart;
		aEditor->InsertTextAt(start, mRemoved.c_str());
		aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 2);
	}

	aEditor->mState = mBefore;
	aEditor->EnsureCursorVisible();

}

void CodeEditor::UndoRecord::Redo(CodeEditor * aEditor)
{
	if (!mRemoved.empty())
	{
		aEditor->DeleteRange(mRemovedStart, mRemovedEnd);
		aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 1);
	}

	if (!mAdded.empty())
	{
		auto start = mAddedStart;
		aEditor->InsertTextAt(start, mAdded.c_str());
		aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 1);
	}

	aEditor->mState = mAfter;
	aEditor->EnsureCursorVisible();
}

static bool TokenizeCStyleString(const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end)
{
	const char * p = in_begin;

	if (*p == '"')
	{
		p++;

		while (p < in_end)
		{
			// handle end of string
			if (*p == '"')
			{
				out_begin = in_begin;
				out_end = p + 1;
				return true;
			}

			// handle escape character for "
			if (*p == '\\' && p + 1 < in_end && p[1] == '"')
				p++;

			p++;
		}
	}

	return false;
}

static bool TokenizeCStyleCharacterLiteral(const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end)
{
	const char * p = in_begin;

	if (*p == '\'')
	{
		p++;

		// handle escape characters
		if (p < in_end && *p == '\\')
			p++;

		if (p < in_end)
			p++;

		// handle end of character literal
		if (p < in_end && *p == '\'')
		{
			out_begin = in_begin;
			out_end = p + 1;
			return true;
		}
	}

	return false;
}

static bool TokenizeCStyleIdentifier(const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end)
{
	const char * p = in_begin;

	if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')
	{
		p++;

		while ((p < in_end) && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
			p++;

		out_begin = in_begin;
		out_end = p;
		return true;
	}

	return false;
}

static bool TokenizeCStyleNumber(const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end)
{
	const char * p = in_begin;

	const bool startsWithNumber = *p >= '0' && *p <= '9';

	if (*p != '+' && *p != '-' && !startsWithNumber)
		return false;

	p++;

	bool hasNumber = startsWithNumber;

	while (p < in_end && (*p >= '0' && *p <= '9'))
	{
		hasNumber = true;

		p++;
	}

	if (hasNumber == false)
		return false;

	bool isFloat = false;
	bool isHex = false;
	bool isBinary = false;

	if (p < in_end)
	{
		if (*p == '.')
		{
			isFloat = true;

			p++;

			while (p < in_end && (*p >= '0' && *p <= '9'))
				p++;
		}
		else if (*p == 'x' || *p == 'X')
		{
			// hex formatted integer of the type 0xef80

			isHex = true;

			p++;

			while (p < in_end && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
				p++;
		}
		else if (*p == 'b' || *p == 'B')
		{
			// binary formatted integer of the type 0b01011101

			isBinary = true;

			p++;

			while (p < in_end && (*p >= '0' && *p <= '1'))
				p++;
		}
	}

	if (isHex == false && isBinary == false)
	{
		// floating point exponent
		if (p < in_end && (*p == 'e' || *p == 'E'))
		{
			isFloat = true;

			p++;

			if (p < in_end && (*p == '+' || *p == '-'))
				p++;

			bool hasDigits = false;

			while (p < in_end && (*p >= '0' && *p <= '9'))
			{
				hasDigits = true;

				p++;
			}

			if (hasDigits == false)
				return false;
		}

		// single precision floating point type
		if (p < in_end && *p == 'f')
			p++;
	}

	if (isFloat == false)
	{
		// integer size type
		while (p < in_end && (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L'))
			p++;
	}

	out_begin = in_begin;
	out_end = p;
	return true;
}

static bool TokenizeCStylePunctuation(const char * in_begin, const char * in_end, const char *& out_begin, const char *& out_end)
{
	(void)in_end;

	switch (*in_begin)
	{
	case '[':
	case ']':
	case '{':
	case '}':
	case '!':
	case '%':
	case '^':
	case '&':
	case '*':
	case '(':
	case ')':
	case '-':
	case '+':
	case '=':
	case '~':
	case '|':
	case '<':
	case '>':
	case '?':
	case ':':
	case '/':
	case ';':
	case ',':
	case '.':
		out_begin = in_begin;
		out_end = in_begin + 1;
		return true;
	}

	return false;
}

// ===========================================================================
// What the C# editor added
// ===========================================================================

static bool IsIdentChar(CodeEditor::Char c)
{
	return std::isalnum(c) || c == '_' || c >= 0x80;
}

std::string CodeEditor::GetLineText(int aLine) const
{
	std::string r;
	if (aLine < 0 || aLine >= (int)mLines.size())
		return r;
	r.reserve(mLines[aLine].size());
	for (auto& g : mLines[aLine])
		r.push_back((char)g.mChar);
	return r;
}

void CodeEditor::JumpTo(const Coordinates& aPosition, bool aFocus)
{
	mStealFocus = aFocus;
	const Coordinates c = SanitizeCoordinates(aPosition);
	mInteractiveStart = mInteractiveEnd = c;
	SetSelection(c, c);
	mState.mCursorPosition = c;
	mCursorPositionChanged = true;
	mCenterOnCursor = true;
	mScrollToCursor = true;
}

void CodeEditor::BeginUndoGroup()
{
	if (mUndoGroupDepth++ == 0)
		mUndoGroupId = ++mUndoGroupCounter;
}

void CodeEditor::EndUndoGroup()
{
	if (mUndoGroupDepth > 0)
		--mUndoGroupDepth;
}

void CodeEditor::ReplaceRange(const Coordinates& aStart, const Coordinates& aEnd, const std::string& aText)
{
	if (mReadOnly)
		return;
	Coordinates start = SanitizeCoordinates(aStart), end = SanitizeCoordinates(aEnd);
	if (end < start)
		std::swap(start, end);
	std::string text;
	text.reserve(aText.size());
	for (char c : aText)
		if (c != '\r')
			text.push_back(c);

	UndoRecord u;
	u.mBefore = mState;
	if (start != end)
	{
		u.mRemoved = GetText(start, end);
		u.mRemovedStart = start;
		u.mRemovedEnd = end;
		DeleteRange(start, end);
	}
	Coordinates pos = start;
	if (!text.empty())
	{
		u.mAdded = text;
		u.mAddedStart = start;
		InsertTextAt(pos, text.c_str());
		u.mAddedEnd = pos;
	}
	if (u.mRemoved.empty() && u.mAdded.empty())
		return;
	mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition = pos;
	mInteractiveStart = mInteractiveEnd = pos;
	u.mAfter = mState;
	AddUndo(u);
	mTextChanged = true;
	mCursorPositionChanged = true;
	Colorize(start.mLine - 1, (pos.mLine - start.mLine) + 3);
	EnsureCursorVisible();
}

// Enter, Tab and the bracket characters, with the help a code editor gives;
// everything else is EnterCharacter's.
void CodeEditor::TypeCharacter(ImWchar aChar, bool aShift)
{
	if (aChar < 128 && aChar != '\n' && aChar != '\t')
		mTyped.push_back((char)aChar);
	if (mLines.empty())
		return;

	const bool sel = HasSelection();
	Coordinates pos = sel ? mState.mSelectionStart : GetActualCursorCoordinates();
	const Line& line = mLines[pos.mLine];
	const int ci = GetCharacterIndex(pos);
	const char next = !sel && ci < (int)line.size() ? (char)line[ci].mChar : '\0';
	const char prev = ci > 0 && ci <= (int)line.size() ? (char)line[ci - 1].mChar : '\0';
	const bool inText = ci > 0 && ci <= (int)line.size() && !IsCodeGlyph(line[ci - 1]) &&
	                    !(line[ci - 1].mChar == '"' || line[ci - 1].mChar == '\'');
	std::string indent;
	for (auto& g : line)
	{
		if (g.mChar != ' ' && g.mChar != '\t')
			break;
		indent.push_back((char)g.mChar);
	}
	const std::string unit = mInsertSpaces ? std::string(mTabSize, ' ') : std::string("\t");

	// Typing the closer that is already there steps over it.
	if (!sel && (aChar == ')' || aChar == ']' || aChar == '}' || aChar == '"' || aChar == '\'') && next == (char)aChar)
	{
		MoveRight(1, false);
		return;
	}

	if (aChar == '\t')
	{
		if (!mInsertSpaces || sel)
		{
			EnterCharacter('\t', aShift);
			return;
		}
		ReplaceRange(pos, pos, std::string(mTabSize - (pos.mColumn % mTabSize), ' '));
		return;
	}

	if (aChar == '\n')
	{
		// The new line keeps the indentation; after `{` one level more, and
		// between `{` and `}` the `}` moves down a line of its own.
		Coordinates end = sel ? mState.mSelectionEnd : pos;
		std::string before = GetText(Coordinates(pos.mLine, 0), pos);
		while (!before.empty() && (before.back() == ' ' || before.back() == '\t'))
			before.pop_back();
		const int endCi = GetCharacterIndex(end);
		const Line& endLine = mLines[end.mLine];
		int skip = endCi;
		while (skip < (int)endLine.size() && (endLine[skip].mChar == ' ' || endLine[skip].mChar == '\t'))
			++skip;
		const char after = skip < (int)endLine.size() ? (char)endLine[skip].mChar : '\0';
		end = Coordinates(end.mLine, GetCharacterColumn(end.mLine, skip));
		if (pos.mColumn < (int)indent.size())
			indent.resize(pos.mColumn);
		if (!mLanguageDefinition.mAutoIndentation)
			indent.clear();
		const char open = before.empty() ? '\0' : before.back();
		const bool opens = open == '{' || open == '(' || open == '[';
		const bool closes = (open == '{' && after == '}') || (open == '(' && after == ')') || (open == '[' && after == ']');
		if (opens && closes)
		{
			ReplaceRange(pos, end, "\n" + indent + unit + "\n" + indent);
			const Coordinates caret(pos.mLine + 1, GetCharacterColumn(pos.mLine + 1, (int)(indent.size() + unit.size())));
			mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition = caret;
			mInteractiveStart = mInteractiveEnd = caret;
			if (!mUndoBuffer.empty())
				mUndoBuffer[mUndoIndex - 1].mAfter = mState;
		}
		else
			ReplaceRange(pos, end, "\n" + indent + (open == '{' ? unit : std::string()));
		return;
	}

	// `}` as the first thing on a line: one level less indentation.
	if (aChar == '}' && !sel && (int)indent.size() == ci && !indent.empty())
	{
		BeginUndoGroup();
		size_t cut = indent.back() == '\t' ? 1 : std::min(indent.size(), unit.size());
		ReplaceRange(Coordinates(pos.mLine, GetCharacterColumn(pos.mLine, (int)(indent.size() - cut))), pos, "");
		EnterCharacter('}', aShift);
		EndUndoGroup();
		return;
	}

	// Openers bring their closer, where a closer makes sense.
	char closer = '\0';
	switch (aChar)
	{
	case '(': closer = ')'; break;
	case '[': closer = ']'; break;
	case '{': closer = '}'; break;
	case '"': closer = '"'; break;
	case '\'': closer = '\''; break;
	default: break;
	}
	if (closer && !sel && !inText)
	{
		const bool roomAfter = next == '\0' || std::isspace((unsigned char)next) || std::strchr(")]};,", next);
		const bool quote = closer == '"' || closer == '\'';
		const bool okBefore = !quote || !(IsIdentChar((Char)prev) || prev == '"' || prev == '\'' || prev == '@' || prev == '$');
		if (roomAfter && okBefore)
		{
			BeginUndoGroup();
			EnterCharacter(aChar, aShift);
			EnterCharacter(closer, false);
			EndUndoGroup();
			MoveLeft(1, false);
			return;
		}
	}

	EnterCharacter(aChar, aShift);
}

void CodeEditor::IndentLines(bool aOutdent)
{
	if (mReadOnly || mLines.empty())
		return;
	Coordinates s = HasSelection() ? mState.mSelectionStart : GetActualCursorCoordinates();
	Coordinates e = HasSelection() ? mState.mSelectionEnd : s;
	const bool hadSelection = HasSelection();
	const Coordinates caret = GetActualCursorCoordinates();
	int last = e.mLine;
	if (hadSelection && e.mColumn == 0 && last > s.mLine)
		--last;
	const std::string unit = mInsertSpaces ? std::string(mTabSize, ' ') : std::string("\t");
	int caretShift = 0;
	BeginUndoGroup();
	for (int l = s.mLine; l <= last; ++l)
	{
		const Line& line = mLines[l];
		if (aOutdent)
		{
			int n = 0;
			if (!line.empty() && line[0].mChar == '\t')
				n = 1;
			else
				while (n < mTabSize && n < (int)line.size() && line[n].mChar == ' ')
					++n;
			if (n > 0)
				ReplaceRange(Coordinates(l, 0), Coordinates(l, GetCharacterColumn(l, n)), "");
			if (l == caret.mLine)
				caretShift = -GetCharacterColumn(l, n);
		}
		else if (!line.empty() || !hadSelection)
		{
			ReplaceRange(Coordinates(l, 0), Coordinates(l, 0), unit);
			if (l == caret.mLine)
				caretShift = mInsertSpaces ? mTabSize : mTabSize;
		}
	}
	EndUndoGroup();
	if (hadSelection)
		SetSelection(Coordinates(s.mLine, 0), Coordinates(last, GetLineMaxColumn(last)));
	else
	{
		const Coordinates c(caret.mLine, std::max(0, caret.mColumn + caretShift));
		mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = SanitizeCoordinates(c);
		SetSelection(mState.mCursorPosition, mState.mCursorPosition);
	}
}

void CodeEditor::ToggleLineComment()
{
	if (mReadOnly || mLines.empty())
		return;
	const bool hadSelection = HasSelection();
	const Coordinates caret = GetActualCursorCoordinates();
	int first = hadSelection ? mState.mSelectionStart.mLine : caret.mLine;
	int last = hadSelection ? mState.mSelectionEnd.mLine : caret.mLine;
	if (hadSelection && mState.mSelectionEnd.mColumn == 0 && last > first)
		--last;

	// Commented when every non-blank line starts with //.
	bool all = true, any = false;
	int minIndent = INT_MAX;
	for (int l = first; l <= last; ++l)
	{
		const Line& line = mLines[l];
		int k = 0;
		while (k < (int)line.size() && (line[k].mChar == ' ' || line[k].mChar == '\t'))
			++k;
		if (k == (int)line.size())
			continue;
		any = true;
		minIndent = std::min(minIndent, k);
		if (!(k + 1 < (int)line.size() && line[k].mChar == '/' && line[k + 1].mChar == '/'))
			all = false;
	}
	if (!any)
		return;
	int caretShift = 0;
	BeginUndoGroup();
	for (int l = first; l <= last; ++l)
	{
		const Line& line = mLines[l];
		int k = 0;
		while (k < (int)line.size() && (line[k].mChar == ' ' || line[k].mChar == '\t'))
			++k;
		if (k == (int)line.size())
			continue;
		if (all)
		{
			int n = 2 + (k + 2 < (int)line.size() && line[k + 2].mChar == ' ' ? 1 : 0);
			const Coordinates a(l, GetCharacterColumn(l, k));
			ReplaceRange(a, Coordinates(l, GetCharacterColumn(l, k + n)), "");
			if (l == caret.mLine && caret.mColumn > a.mColumn)
				caretShift = -std::min(n, caret.mColumn - a.mColumn);
		}
		else
		{
			const Coordinates a(l, GetCharacterColumn(l, minIndent));
			ReplaceRange(a, a, "// ");
			if (l == caret.mLine && caret.mColumn >= a.mColumn)
				caretShift = 3;
		}
	}
	EndUndoGroup();
	if (hadSelection)
		SetSelection(Coordinates(first, 0), Coordinates(last, GetLineMaxColumn(last)));
	else
	{
		const Coordinates c = SanitizeCoordinates(Coordinates(caret.mLine, std::max(0, caret.mColumn + caretShift)));
		mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = c;
		SetSelection(c, c);
	}
}

int CodeEditor::ByteOfChar(int aLine, int aCh) const
{
	if (aLine < 0 || aLine >= (int)mLines.size())
		return 0;
	const Line& line = mLines[aLine];
	int b = 0;
	for (int c = 0; c < aCh && b < (int)line.size(); ++c)
		b += UTF8CharLength(line[b].mChar);
	return std::min(b, (int)line.size());
}

CodeEditor::Coordinates CodeEditor::FromTextPos(int aLine, int aCh) const
{
	if (mLines.empty())
		return Coordinates();
	aLine = std::max(0, std::min(aLine, (int)mLines.size() - 1));
	return Coordinates(aLine, GetCharacterColumn(aLine, ByteOfChar(aLine, std::max(0, aCh))));
}

CodeEditor::TextPos CodeEditor::ToTextPos(const Coordinates& aCoords) const
{
	TextPos p;
	const Coordinates c = SanitizeCoordinates(aCoords);
	p.line = c.mLine;
	if (c.mLine >= (int)mLines.size())
		return p;
	const int end = GetCharacterIndex(c);
	const Line& line = mLines[c.mLine];
	for (int b = 0; b < end && b < (int)line.size(); b += UTF8CharLength(line[b].mChar))
		++p.ch;
	return p;
}

void CodeEditor::SetSemanticTokens(const std::vector<SemanticToken>& aTokens)
{
	for (auto& line : mLines)
		for (auto& g : line)
			g.mSemantic = PaletteIndex::Default;
	for (const SemanticToken& t : aTokens)
	{
		if (t.line < 0 || t.line >= (int)mLines.size())
			continue;
		Line& line = mLines[t.line];
		const int b0 = ByteOfChar(t.line, t.ch), b1 = ByteOfChar(t.line, t.ch + t.len);
		for (int b = b0; b < b1 && b < (int)line.size(); ++b)
			line[b].mSemantic = t.color;
	}
}

void CodeEditor::SetSquiggles(const std::vector<Squiggle>& aSquiggles)
{
	for (auto& line : mLines)
		for (auto& g : line)
			g.mDiag = 0;
	for (const Squiggle& q : aSquiggles)
	{
		const int l0 = std::max(0, q.start.line), l1 = std::min(q.end.line, (int)mLines.size() - 1);
		for (int l = l0; l <= l1; ++l)
		{
			Line& line = mLines[l];
			int b0 = l == q.start.line ? ByteOfChar(l, q.start.ch) : 0;
			int b1 = l == q.end.line ? ByteOfChar(l, q.end.ch) : (int)line.size();
			if (b0 >= b1 && l0 == l1)
			{
				// An empty range ("; expected"): the character there, or the one before.
				if (b0 < (int)line.size())
					b1 = b0 + UTF8CharLength(line[b0].mChar);
				else if (b0 > 0)
				{
					b1 = b0;
					b0 = b0 - 1;
					while (b0 > 0 && IsUTFSequence(line[b0].mChar))
						--b0;
				}
			}
			for (int b = b0; b < b1 && b < (int)line.size(); ++b)
				line[b].mDiag = (uint8_t)std::max<int>(line[b].mDiag, q.severity);
		}
	}
}

ImVec2 CodeEditor::CoordinatesToScreen(const Coordinates& aCoords) const
{
	if (mLines.empty())
		return mOrigin;
	const Coordinates c = SanitizeCoordinates(aCoords);
	return ImVec2(mOrigin.x + mTextStart + TextDistanceToLineStart(c), mOrigin.y + c.mLine * mCharAdvance.y);
}

bool CodeEditor::ScreenToCoordinates(const ImVec2& aPos, Coordinates& aOut) const
{
	if (aPos.x < mViewMin.x || aPos.y < mViewMin.y || aPos.x >= mViewMax.x || aPos.y >= mViewMax.y)
		return false;
	if (aPos.x < mOrigin.x + mTextStart || mCharAdvance.y <= 0.0f)
		return false;
	const int lineNo = (int)std::floor((aPos.y - mOrigin.y) / mCharAdvance.y);
	if (lineNo < 0 || lineNo >= (int)mLines.size())
		return false;
	const float endX = CoordinatesToScreen(Coordinates(lineNo, GetLineMaxColumn(lineNo))).x;
	if (aPos.x > endX)
		return false;
	aOut = ScreenPosToCoordinates(aPos);
	return true;
}

bool CodeEditor::GetIdentifierAt(const Coordinates& aAt, Coordinates& aStart, Coordinates& aEnd) const
{
	const Coordinates at = SanitizeCoordinates(aAt);
	if (at.mLine >= (int)mLines.size())
		return false;
	const Line& line = mLines[at.mLine];
	int ci = GetCharacterIndex(at);
	if (!(ci < (int)line.size() && IsIdentChar(line[ci].mChar)))
	{
		if (ci > 0 && IsIdentChar(line[ci - 1].mChar))
			--ci;
		else
			return false;
	}
	int s = ci, e = ci;
	while (s > 0 && IsIdentChar(line[s - 1].mChar))
		--s;
	while (e < (int)line.size() && IsIdentChar(line[e].mChar))
		++e;
	if (std::isdigit(line[s].mChar))
		return false;
	aStart = Coordinates(at.mLine, GetCharacterColumn(at.mLine, s));
	aEnd = Coordinates(at.mLine, GetCharacterColumn(at.mLine, e));
	return true;
}

bool CodeEditor::IsCodeGlyph(const Glyph& aGlyph) const
{
	if (aGlyph.mComment || aGlyph.mMultiLineComment)
		return false;
	return aGlyph.mColorIndex != PaletteIndex::String && aGlyph.mColorIndex != PaletteIndex::CharLiteral;
}

bool CodeEditor::FindMatchingBracket(const Coordinates& aAt, Coordinates& aOpen, Coordinates& aClose) const
{
	if (mLines.empty())
		return false;
	const Coordinates at = SanitizeCoordinates(aAt);
	const Line& line = mLines[at.mLine];
	const int ci = GetCharacterIndex(at);
	auto isBracket = [](char c) { return c && std::strchr("()[]{}", c) != nullptr; };
	int bi = -1;
	if (ci < (int)line.size() && isBracket((char)line[ci].mChar) && IsCodeGlyph(line[ci]))
		bi = ci;
	else if (ci > 0 && ci <= (int)line.size() && isBracket((char)line[ci - 1].mChar) && IsCodeGlyph(line[ci - 1]))
		bi = ci - 1;
	if (bi < 0)
		return false;
	const char c = (char)line[bi].mChar;
	const char* pairs = "()[]{}";
	const int idx = (int)(std::strchr(pairs, c) - pairs);
	const bool forward = (idx % 2) == 0;
	const char open = pairs[idx & ~1], close = pairs[idx | 1];
	int depth = 0, l = at.mLine, i = bi, budget = 200000;
	while (budget-- > 0)
	{
		const Line& ln = mLines[l];
		if (i >= 0 && i < (int)ln.size() && IsCodeGlyph(ln[i]))
		{
			const char g = (char)ln[i].mChar;
			if (g == open) depth += forward ? 1 : -1;
			else if (g == close) depth += forward ? -1 : 1;
			if (depth == 0)
			{
				const Coordinates a(at.mLine, GetCharacterColumn(at.mLine, bi)), b(l, GetCharacterColumn(l, i));
				aOpen = forward ? a : b;
				aClose = forward ? b : a;
				return true;
			}
		}
		if (forward)
		{
			if (++i >= (int)ln.size())
			{
				if (++l >= (int)mLines.size())
					return false;
				i = 0;
				if (mLines[l].empty())
					i = -1;
			}
		}
		else
		{
			if (--i < 0)
			{
				do
				{
					if (--l < 0)
						return false;
				} while (mLines[l].empty());
				i = (int)mLines[l].size() - 1;
			}
		}
	}
	return false;
}

// --- C# ---------------------------------------------------------------------------
static bool TokenizeCSharpString(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	const char* p = in_begin;
	bool verbatim = false;
	while (p < in_end && (*p == '@' || *p == '$'))
	{
		verbatim |= *p == '@';
		++p;
	}
	if (p >= in_end || *p != '"')
		return false;
	++p;
	while (p < in_end)
	{
		if (*p == '"')
		{
			if (verbatim && p + 1 < in_end && p[1] == '"')
			{
				p += 2;
				continue;
			}
			out_begin = in_begin;
			out_end = p + 1;
			return true;
		}
		if (!verbatim && *p == '\\' && p + 1 < in_end)
			++p;
		++p;
	}
	// Unterminated: the rest of the line.
	out_begin = in_begin;
	out_end = in_end;
	return true;
}

static bool TokenizeCSharpNumber(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end)
{
	const char* p = in_begin;
	if (!(*p >= '0' && *p <= '9'))
		return false;
	if (*p == '0' && p + 1 < in_end && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B'))
	{
		p += 2;
		while (p < in_end && (std::isxdigit((unsigned char)*p) || *p == '_'))
			++p;
	}
	else
	{
		while (p < in_end && ((*p >= '0' && *p <= '9') || *p == '_'))
			++p;
		if (p + 1 < in_end && *p == '.' && p[1] >= '0' && p[1] <= '9')
		{
			++p;
			while (p < in_end && ((*p >= '0' && *p <= '9') || *p == '_'))
				++p;
		}
		if (p < in_end && (*p == 'e' || *p == 'E'))
		{
			const char* q = p + 1;
			if (q < in_end && (*q == '+' || *q == '-'))
				++q;
			if (q < in_end && *q >= '0' && *q <= '9')
			{
				p = q;
				while (p < in_end && *p >= '0' && *p <= '9')
					++p;
			}
		}
	}
	while (p < in_end && std::strchr("fFdDmMuUlL", *p) && *p)
		++p;
	out_begin = in_begin;
	out_end = p;
	return true;
}

static bool TokenizeCSharp(const char* in_begin, const char* in_end, const char*& out_begin, const char*& out_end,
                           CodeEditor::PaletteIndex& paletteIndex)
{
	paletteIndex = CodeEditor::PaletteIndex::Max;
	while (in_begin < in_end && isascii(*in_begin) && isblank(*in_begin))
		in_begin++;
	if (in_begin == in_end)
	{
		out_begin = in_end;
		out_end = in_end;
		paletteIndex = CodeEditor::PaletteIndex::Default;
	}
	else if (TokenizeCSharpString(in_begin, in_end, out_begin, out_end))
		paletteIndex = CodeEditor::PaletteIndex::String;
	else if (TokenizeCStyleCharacterLiteral(in_begin, in_end, out_begin, out_end))
		paletteIndex = CodeEditor::PaletteIndex::CharLiteral;
	else if (*in_begin == '@' && in_begin + 1 < in_end && IsIdentChar((CodeEditor::Char)in_begin[1]))
	{
		const char* p = in_begin + 1;
		while (p < in_end && IsIdentChar((CodeEditor::Char)*p))
			++p;
		out_begin = in_begin;
		out_end = p;
		paletteIndex = CodeEditor::PaletteIndex::Default;   // @class: a name, not a keyword
	}
	else if ((std::isalpha((unsigned char)*in_begin) || *in_begin == '_' || (unsigned char)*in_begin >= 0x80))
	{
		const char* p = in_begin + 1;
		while (p < in_end && IsIdentChar((CodeEditor::Char)*p))
			++p;
		out_begin = in_begin;
		out_end = p;
		paletteIndex = CodeEditor::PaletteIndex::Identifier;
	}
	else if (TokenizeCSharpNumber(in_begin, in_end, out_begin, out_end))
		paletteIndex = CodeEditor::PaletteIndex::Number;
	else if (TokenizeCStylePunctuation(in_begin, in_end, out_begin, out_end))
		paletteIndex = CodeEditor::PaletteIndex::Punctuation;

	return paletteIndex != CodeEditor::PaletteIndex::Max;
}

const CodeEditor::LanguageDefinition& CodeEditor::LanguageDefinition::CSharp()
{
	static bool inited = false;
	static LanguageDefinition langDef;
	if (!inited)
	{
		static const char* const keywords[] = {
			"abstract", "as", "base", "bool", "byte", "char", "checked", "class", "const", "decimal", "delegate",
			"double", "enum", "event", "explicit", "extern", "false", "fixed", "float", "implicit", "in", "int",
			"interface", "internal", "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out",
			"override", "params", "private", "protected", "public", "readonly", "ref", "sbyte", "sealed", "short",
			"sizeof", "stackalloc", "static", "string", "struct", "this", "true", "typeof", "uint", "ulong",
			"unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile",
			// contextual
			"var", "dynamic", "async", "get", "set", "init", "partial", "where", "nameof", "record", "when",
			"with", "not", "and", "or", "global", "required", "nint", "nuint", "scoped", "file",
		};
		for (auto& k : keywords)
			langDef.mKeywords.insert(k);
		static const char* const control[] = {
			"if", "else", "switch", "case", "default", "for", "foreach", "while", "do", "break", "continue",
			"return", "goto", "throw", "try", "catch", "finally", "yield", "await",
		};
		for (auto& k : control)
			langDef.mControlKeywords.insert(k);

		langDef.mTokenize = &TokenizeCSharp;
		langDef.mCommentStart = "/*";
		langDef.mCommentEnd = "*/";
		langDef.mSingleLineComment = "//";
		langDef.mCaseSensitive = true;
		langDef.mAutoIndentation = true;
		langDef.mName = "C#";
		inited = true;
	}
	return langDef;
}
