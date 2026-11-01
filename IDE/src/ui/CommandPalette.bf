using System;
using System.Collections;
using Beefy;
using Beefy.widgets;
using Beefy.events;
using Beefy.theme.dark;
using Beefy.gfx;
using Beefy.geom;

namespace IDE.ui
{
	// A VS Code-style command palette: a search box over a filtered list of every IDE command. Opened with
	// Ctrl+Shift+P ("Show Command Palette"). Type to filter, Up/Down to move, Enter to run, Escape to close.
	class CommandPalette : Widget
	{
		class Entry
		{
			public String mName ~ delete _;
			public String mShortcut ~ delete _; // Empty when the command has no key binding
			public int32 mScore; // Fuzzy match score; higher ranks first (0 when the query is empty)
		}

		public const int32 cMaxVisibleRows = 12;

		DarkEditWidget mEditWidget;
		List<Entry> mEntries = new .() ~ DeleteContainerAndItems!(_);
		int32 mSelectIdx = -1;
		int32 mScrollIdx;
		float mPadding;
		float mEditHeight;
		float mRowHeight;
		bool mClosed;

		public this()
		{
			mPadding = GS!(6);
			mRowHeight = DarkTheme.sDarkTheme.mSmallFont.GetLineSpacing() + GS!(2);
			mEditHeight = GS!(24);
		}

		public static void Show()
		{
			if (gApp.mCommandPalette != null)
			{
				// Toggle off if it's already open.
				gApp.mCommandPalette.Close();
				return;
			}

			var palette = new CommandPalette();
			palette.Init();
		}

		void Init()
		{
			gApp.mCommandPalette = this;
			// The owning window deletes us on close; just drop the app reference then.
			mOnRemovedFromParent.Add(new (widget, prevParent, widgetWindow) =>
				{
					if (gApp.mCommandPalette == this)
						gApp.mCommandPalette = null;
				});

			mEditWidget = new DarkEditWidget();
			mEditWidget.mDrawBox = false;
			mEditWidget.mOnKeyDown.Add(new => HandleEditKeyDown);
			mEditWidget.mOnSubmit.Add(new => HandleSubmit);
			mEditWidget.mOnContentChanged.Add(new => HandleContentChanged);
			AddWidget(mEditWidget);

			// Close when focus leaves the palette (eg the user clicks back into the editor).
			WidgetWindow.sOnWindowLostFocus.Add(new => HandleWindowLostFocus);

			Rebuild();

			float width = GS!(600);
			float height = ComputeHeight();

			WidgetWindow parentWindow = gApp.GetActiveWindow() as WidgetWindow;
			if (parentWindow == null)
				parentWindow = gApp.mMainWindow;

			int32 x = 0;
			int32 y = 0;
			if (parentWindow != null)
			{
				x = parentWindow.mClientX + (parentWindow.mClientWidth - (int32)width) / 2;
				y = parentWindow.mClientY + GS!(80);
			}

			// An activatable (not FakeFocus/NoActivate) popup so the edit box receives typed characters.
			mWidgetWindow = new WidgetWindow(parentWindow, "Command Palette", x, y, (int32)width, (int32)height,
				.ClientSized | .PopupPosition, this);
			mWidgetWindow.SetFocus(mEditWidget);
		}

		void HandleWindowLostFocus(BFWindow window, BFWindow newFocus)
		{
			if ((mWidgetWindow != null) && (window == mWidgetWindow))
				Close();
		}

		public void Close()
		{
			if (mClosed)
				return;
			mClosed = true;
			WidgetWindow.sOnWindowLostFocus.Remove(scope => HandleWindowLostFocus, true);
			if (mWidgetWindow != null)
				mWidgetWindow.Close();
		}

		float ComputeHeight()
		{
			return mPadding + mEditHeight + mPadding + cMaxVisibleRows * mRowHeight + mPadding;
		}

		public override void Resize(float x, float y, float width, float height)
		{
			base.Resize(x, y, width, height);
			if (mEditWidget != null)
				mEditWidget.Resize(mPadding, mPadding, width - mPadding * 2, mEditHeight);
		}

		int32 GetVisibleRows()
		{
			float listTop = mPadding + mEditHeight + mPadding;
			return Math.Max(1, (int32)((mHeight - listTop - mPadding) / mRowHeight));
		}

		// ---- Filtering ----

		void HandleContentChanged(EditEvent evt)
		{
			Rebuild();
		}

		void Rebuild()
		{
			String query = scope String();
			if (mEditWidget != null)
				mEditWidget.GetText(query);
			Rebuild(query);
		}

		// IDEHelper/third_party/FtsFuzzyMatch.h — the same fuzzy matcher autocomplete uses.
		[CallingConvention(.Stdcall), CLink]
		static extern bool fts_fuzzy_match(char8* pattern, char8* str, ref int32 outScore, uint8* matches, int maxMatches);

		void Rebuild(StringView queryView)
		{
			ClearAndDeleteItems(mEntries);

			String query = scope String(queryView);
			query.Trim();

			for (var cmd in gApp.mCommands.mCommandMap.Values)
			{
				String name = cmd.mName;

				var entry = new Entry();
				entry.mName = new String(name);
				String shortcut = scope String();
				cmd.ToString(shortcut);
				entry.mShortcut = new String(shortcut);

				if (!query.IsEmpty)
				{
					int32 score = 0;
					uint8[256] matches = ?;
					if (!fts_fuzzy_match(query.CStr(), name.CStr(), ref score, &matches, matches.Count))
					{
						delete entry;
						continue;
					}
					entry.mScore = score;
				}

				mEntries.Add(entry);
			}

			// Higher fuzzy score first; ties broken alphabetically.
			mEntries.Sort(scope (lhs, rhs) =>
				{
					if (lhs.mScore != rhs.mScore)
						return lhs.mScore > rhs.mScore ? -1 : 1;
					return String.Compare(lhs.mName, rhs.mName, true);
				});

			if (mEntries.Count > 0)
			{
				if (mSelectIdx < 0)
					mSelectIdx = 0;
				mSelectIdx = Math.Clamp(mSelectIdx, 0, (int32)mEntries.Count - 1);
			}
			else
				mSelectIdx = -1;
			mScrollIdx = 0;
			EnsureVisible();
		}

		// ---- Selection ----

		void MoveSelection(int32 delta)
		{
			if (mEntries.Count == 0)
			{
				mSelectIdx = -1;
				return;
			}
			mSelectIdx = Math.Clamp(mSelectIdx + delta, 0, (int32)mEntries.Count - 1);
			EnsureVisible();
		}

		void EnsureVisible()
		{
			if (mSelectIdx < 0)
				return;
			int32 visRows = GetVisibleRows();
			if (mSelectIdx < mScrollIdx)
				mScrollIdx = mSelectIdx;
			if (mSelectIdx >= mScrollIdx + visRows)
				mScrollIdx = mSelectIdx - visRows + 1;
			mScrollIdx = Math.Clamp(mScrollIdx, 0, Math.Max(0, (int32)mEntries.Count - visRows));
		}

		void HandleSubmit(EditEvent evt)
		{
			RunSelected();
		}

		void RunSelected()
		{
			if ((mSelectIdx < 0) || (mSelectIdx >= mEntries.Count))
				return;

			String name = scope String(mEntries[mSelectIdx].mName);
			IDECommand cmd;
			bool found = gApp.mCommands.mCommandMap.TryGetValue(name, out cmd);
			Close();
			if (found)
				cmd.mAction();
		}

		// ---- Input ----

		void HandleEditKeyDown(KeyDownEvent evt)
		{
			if (evt.mKeyCode == .Up)
			{
				MoveSelection(-1);
				evt.mHandled = true;
			}
			else if (evt.mKeyCode == .Down)
			{
				MoveSelection(1);
				evt.mHandled = true;
			}
			else if (evt.mKeyCode == .PageUp)
			{
				MoveSelection(-GetVisibleRows());
				evt.mHandled = true;
			}
			else if (evt.mKeyCode == .PageDown)
			{
				MoveSelection(GetVisibleRows());
				evt.mHandled = true;
			}
			else if (evt.mKeyCode == .Escape)
			{
				evt.mHandled = true;
				Close();
			}
		}

		int32 GetRowAt(float y)
		{
			float listTop = mPadding + mEditHeight + mPadding;
			if (y < listTop)
				return -1;
			int32 row = (int32)((y - listTop) / mRowHeight);
			if (row < 0)
				return -1;
			int32 idx = mScrollIdx + row;
			if (idx >= mEntries.Count)
				return -1;
			return idx;
		}

		public override void MouseDown(float x, float y, int32 btn, int32 btnCount)
		{
			base.MouseDown(x, y, btn, btnCount);
			int32 idx = GetRowAt(y);
			if (idx != -1)
			{
				mSelectIdx = idx;
				RunSelected();
			}
		}

		public override void MouseWheel(float x, float y, float deltaX, float deltaY)
		{
			base.MouseWheel(x, y, deltaX, deltaY);
			if (deltaY == 0)
				return;
			MoveSelection(deltaY > 0 ? -1 : 1);
		}

		// ---- Drawing ----

		public override void Draw(Graphics g)
		{
			using (g.PushColor(DarkTheme.COLOR_WINDOW))
				g.FillRect(0, 0, mWidth, mHeight);
			using (g.PushColor(DarkTheme.COLOR_DIALOG_OUTLINE_OUT))
				g.OutlineRect(0, 0, mWidth, mHeight);

			var font = DarkTheme.sDarkTheme.mSmallFont;
			g.SetFont(font);

			float listLeft = mPadding;
			float listTop = mPadding + mEditHeight + mPadding;
			float listWidth = mWidth - mPadding * 2;

			int32 visRows = GetVisibleRows();
			for (int32 i = 0; i < visRows; i++)
			{
				int32 idx = mScrollIdx + i;
				if (idx >= mEntries.Count)
					break;

				var entry = mEntries[idx];
				float rowY = listTop + i * mRowHeight;

				if (idx == mSelectIdx)
				{
					using (g.PushColor(DarkTheme.COLOR_TEXT_SELECTED))
						g.FillRect(listLeft, rowY, listWidth, mRowHeight);
				}

				using (g.PushColor(DarkTheme.COLOR_TEXT))
					g.DrawString(entry.mName, listLeft + GS!(4), rowY + GS!(1));

				if (!entry.mShortcut.IsEmpty)
				{
					float shortcutWidth = font.GetWidth(entry.mShortcut);
					using (g.PushColor(DarkTheme.COLOR_TEXT_DISABLED))
						g.DrawString(entry.mShortcut, listLeft + listWidth - shortcutWidth - GS!(4), rowY + GS!(1));
				}
			}
		}
	}
}
