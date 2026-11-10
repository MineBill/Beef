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
	// The shared VS Code-style palette popup: a borderless window with a search box over a filtered,
	// scrollable list. The command palette and the Find dialog are both built on this. Subclasses
	// fill in the entries for a query and decide what happens when one is run. Type to filter,
	// Up/Down/PageUp/PageDown to move, Enter to run, Escape to close.
	class PaletteWidget : Widget
	{
		public class Entry
		{
			public String mName ~ delete _; // Primary text, drawn on the left
			public String mDetail ~ delete _; // Secondary text, drawn right-aligned. May be empty
			public int32 mScore; // Fuzzy match score; higher ranks first (0 when the query is empty)
		}

		public const int32 cMaxVisibleRows = 12;

		protected DarkEditWidget mEditWidget;
		protected List<Entry> mEntries = new .() ~ DeleteContainerAndItems!(_);
		protected int32 mSelectIdx = -1;
		protected int32 mScrollIdx;
		protected float mPadding;
		protected float mEditHeight;
		protected float mRowHeight;
		protected bool mClosed;

		public this()
		{
			mPadding = GS!(8);
			mRowHeight = DarkTheme.sDarkTheme.mSmallFont.GetLineSpacing() + GS!(2);
			mEditHeight = GS!(24);
		}

		// ---- Subclass interface ----

		// Fills mEntries with the results for the trimmed query.
		protected virtual void RebuildEntries(StringView query)
		{
		}

		// Called after the palette is closed, so the action is free to open another window
		protected virtual void RunEntry(Entry entry)
		{
		}

		protected virtual float GetPreferredWidth()
		{
			return GS!(600);
		}

		// ---- Setup ----

		protected void InitPalette(String title, String initialText = null)
		{
			mEditWidget = new DarkEditWidget();
			mEditWidget.mDrawBox = true;
			mEditWidget.mOnKeyDown.Add(new => HandleEditKeyDown);
			mEditWidget.mOnSubmit.Add(new => HandleSubmit);
			mEditWidget.mOnContentChanged.Add(new => HandleContentChanged);
			AddWidget(mEditWidget);

			// Close when focus leaves the palette (eg the user clicks back into the editor).
			WidgetWindow.sOnWindowLostFocus.Add(new => HandleWindowLostFocus);

			if (initialText != null)
			{
				// Select everything so typing replaces the previous search.
				mEditWidget.SetText(initialText);
				mEditWidget.Content.SelectAll();
			}

			Rebuild();

			float width = GetPreferredWidth();
			float height = ComputeHeight();

			WidgetWindow parentWindow = gApp.GetActiveWindow();
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
			mWidgetWindow = new WidgetWindow(parentWindow, title, x, y, (int32)width, (int32)height,
				.ClientSized | .PopupPosition, this);
			mWidgetWindow.SetFocus(mEditWidget);
		}

		void HandleWindowLostFocus(BFWindow window, BFWindow newFocus)
		{
			if ((mWidgetWindow != null) && (window == mWidgetWindow))
				Close();
		}

		public virtual void Close()
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
			EnsureVisible();
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

		protected void Rebuild()
		{
			String query = scope String();
			if (mEditWidget != null)
				mEditWidget.GetText(query);
			Rebuild(query);
		}

		// IDEHelper/third_party/FtsFuzzyMatch.h — the same fuzzy matcher autocomplete uses.
		[CallingConvention(.Stdcall), CLink]
		static extern bool fts_fuzzy_match(char8* pattern, char8* str, ref int32 outScore, uint8* matches, int maxMatches);

		protected static bool FuzzyMatch(String query, String name, out int32 score)
		{
			score = 0;
			uint8[256] matches = ?;
			return fts_fuzzy_match(query.CStr(), name.CStr(), ref score, &matches, matches.Count);
		}

		protected void Rebuild(StringView queryView)
		{
			ClearAndDeleteItems(mEntries);

			String query = scope String(queryView);
			query.Trim();

			RebuildEntries(query);

			// Higher fuzzy score first; ties broken alphabetically, then by detail so entries with
			//  the same name (eg files in different folders) keep a stable order.
			mEntries.Sort(scope (lhs, rhs) =>
				{
					if (lhs.mScore != rhs.mScore)
						return lhs.mScore > rhs.mScore ? -1 : 1;
					int nameCmp = String.Compare(lhs.mName, rhs.mName, true);
					if (nameCmp != 0)
						return nameCmp;
					if ((lhs.mDetail == null) || (rhs.mDetail == null))
						return 0;
					return String.Compare(lhs.mDetail, rhs.mDetail, true);
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
			MarkDirty();
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
			MarkDirty();
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

			Entry entry = mEntries[mSelectIdx];
			Close();
			RunEntry(entry);
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
			float listLeft = mPadding;
			float listTop = mPadding + mEditHeight + mPadding;
			float listWidth = mWidth - mPadding * 2;
			float listHeight = mHeight - listTop - mPadding;

			using (g.PushColor(DarkTheme.COLOR_BKG))
				g.FillRect(0, 0, mWidth, mHeight);
			using (g.PushColor(DarkTheme.COLOR_WINDOW))
				g.FillRect(listLeft, listTop, listWidth, listHeight);
			using (g.PushColor(DarkTheme.COLOR_DIALOG_OUTLINE_OUT))
				g.OutlineRect(0, 0, mWidth, mHeight, 2);

			var font = DarkTheme.sDarkTheme.mSmallFont;
			g.SetFont(font);

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

				if (!entry.mDetail.IsEmpty)
				{
					float detailWidth = font.GetWidth(entry.mDetail);
					using (g.PushColor(DarkTheme.COLOR_TEXT_DISABLED))
						g.DrawString(entry.mDetail, listLeft + listWidth - detailWidth - GS!(4), rowY + GS!(1));
				}

				using (g.PushColor(DarkTheme.COLOR_TEXT))
					g.DrawString(entry.mName, listLeft + GS!(4), rowY + GS!(1));
			}
		}
	}
}
