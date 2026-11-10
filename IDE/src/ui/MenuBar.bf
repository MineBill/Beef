using Beefy;
using Beefy.gfx;
using Beefy.widgets;
using Beefy.theme.dark;
using Beefy.geom;
using IDE;
using IDE.util;
using System;
using System.Diagnostics;
using System.Collections;
using Beefy.sys;

namespace IDE.ui;

public class MenuBar : Widget
{
	class UISysMenu : SysMenu
	{
		public override SysMenu AddMenuItem(String text, String hotKey = null, MenuItemSelectedHandler menuItemSelectedHandler = null,
			MenuItemUpdateHandler menuItemUpdateHandler = null, SysBitmap bitmap = null, bool enabled = true, int32 checkState = -1, bool radioCheck = false)
		{
			if (mChildren == null)
			    mChildren = new List<SysMenu>();

			UISysMenu sysMenu = new UISysMenu();
			if (text != null)
			{
				sysMenu.mText = new String(text);
			}
			if ((hotKey != null) && (hotKey.Length != 0) && ((hotKey.Length > 1 ) || (hotKey[0] != '#')))
			{
				sysMenu.mHotKey = new String(hotKey);
				sysMenu.mHotKey.TrimStart('#');
			}

			sysMenu.mBitmap = bitmap;
			sysMenu.mEnabled = enabled;
			sysMenu.mCheckState = checkState;
			sysMenu.mRadioCheck = radioCheck;
			if (menuItemSelectedHandler != null)
				sysMenu.mOnMenuItemSelected.Add(menuItemSelectedHandler);
			if (menuItemUpdateHandler != null)
				sysMenu.mOnMenuItemUpdate.Add(menuItemUpdateHandler);
			sysMenu.mNativeBFMenu = null;
			sysMenu.mParent = this;
			sysMenu.mWindow = mWindow;
			mChildren.Add(sysMenu);

			return sysMenu;
		}

		public override void Modify(String text, String hotKey = null, SysBitmap bitmap = null, bool enabled = true, int32 checkState = -1, bool radioCheck = false)
		{
			if (((Object)mText != text) && (text != null))
			{
				if (text.Length > 0)
				{
					String.NewOrSet!(mText, text);
				}
				else
					DeleteAndNullify!(mText);
			}

			if (((Object)mHotKey != hotKey) && (hotKey != null))
			{
				if ((hotKey.Length > 0) && ((hotKey.Length > 1 ) || (hotKey[0] != '#')))
				{
					String.NewOrSet!(mHotKey, hotKey);
					mHotKey.TrimStart('#');
				}
				else
					DeleteAndNullify!(mHotKey);
			}
			mBitmap = bitmap;
			mEnabled = enabled;
			mCheckState = checkState;
			mRadioCheck = radioCheck;
		}
	}

	public class UISysMenuRoot : UISysMenu
	{
		public MenuBar mRootMenuBar;
		public this()
		{

		}

		public override SysMenu AddMenuItem(String text, String hotKey = null, MenuItemSelectedHandler menuItemSelectedHandler = null, MenuItemUpdateHandler menuItemUpdateHandler = null,
			SysBitmap bitmap = null, bool enabled = true, int32 checkState = -1, bool radioCheck = false)
		{
			let item =  base.AddMenuItem(text, hotKey, menuItemSelectedHandler, menuItemUpdateHandler, bitmap, enabled, checkState, radioCheck);
			let button = new Button();
			button.Label = text;
			button.mSysMenu = item;
			button.mMenuBar = mRootMenuBar;
			button.mOnMouseClick.Add(new (evt) => {
				mOnMenuItemSelected(this);
				mRootMenuBar.ShowMenu(button);
			});
			mRootMenuBar.AddItemButton(button);
			return item;
		}
	}

	public class Button : ButtonWidget
	{
		public MenuBar mMenuBar;
		public MenuWidget mMenuWidget;
		private String mLabel ~ delete _;

		public StringView Label
		{
			get
			{
				return mLabel;
			}

			set
			{
				String.NewOrSet!(mLabel, value);
			}
		}

		public SysMenu mSysMenu;

		public KeyCode MenuKey => DarkMenuWidget.GetMenuKey(mLabel);
		
		public override void Draw(Graphics g)
		{
			base.Draw(g);

			if (mMouseOver)
			{
				using (g.PushColor(Color.Mult(DarkTheme.COLOR_TEXT, Color.Get(0.12f))))
				{
					g.FillRect(0, 0, mWidth, mHeight);
				}
			}

			g.SetFont(DarkTheme.sDarkTheme.mSmallFont);
			if (mLabel != null)
			{
			    using (g.PushColor(mDisabled ? 0x80FFFFFF : Color.White))
			    {
					using (g.PushColor(DarkTheme.COLOR_TEXT))
						DarkTheme.DrawUnderlined(g, mLabel, GS!(2), (mHeight - GS!(20)) / 2, .Centered, mWidth - GS!(4), .Truncate);
			    }
			}
		}

		public override void MouseEnter()
		{
			base.MouseEnter();

			// If we already have a different menu open then we should open our menu now
			for (var button in mMenuBar.mButtons)
			{
				if (button.mMenuWidget != null)
				{
					if (button == this)
					{
						// Already opened
						return;
					}

					// Close the old menu, open the new ones
					button.mMenuWidget.Close();
					mMenuBar.ShowMenu(this);
					return;
				}
			}
		}
	}


	public enum WindowButtonKind
	{
		Minimize,
		MaximizeRestore,
		Close
	}

	public class WindowButton : Widget
	{
		public WindowButtonKind mKind;

		public this(WindowButtonKind kind)
		{
			mKind = kind;
		}

		bool IsMaximized
		{
			get
			{
				var window = mWidgetWindow;
				if (window == null)
					return false;
				return (window.mShowKind == .Maximized) || (window.mShowKind == .ShowMaximized);
			}
		}

		public override void Draw(Graphics g)
		{
			bool active = (mWidgetWindow != null) && (mWidgetWindow.mHasFocus);
			bool isClose = mKind == .Close;

			if ((mMouseOver) || (mMouseDown))
			{
				if (isClose)
				{
					using (g.PushColor(mMouseDown ? 0xFFC1121F : 0xFFE81123))
						g.FillRect(0, 0, mWidth, mHeight);
				}
				else
				{
					using (g.PushColor(Color.Mult(DarkTheme.COLOR_TEXT, Color.Get(mMouseDown ? 0.16f : 0.08f))))
						g.FillRect(0, 0, mWidth, mHeight);
				}
			}

			uint32 glyphColor = (isClose && (mMouseOver || mMouseDown)) ? Color.White : (active ? DarkTheme.COLOR_TEXT : DarkTheme.COLOR_TEXT_DISABLED);

			float cx = mWidth / 2;
			float cy = mHeight / 2;

			using (g.PushColor(glyphColor))
			{
				switch (mKind)
				{
				case .Minimize:
					g.DrawLine(cx - GS!(5), cy + GS!(3), cx + GS!(5), cy + GS!(3), GS!(1));
				case .MaximizeRestore:
					if (IsMaximized)
					{
						// Restore: two overlapping squares
						g.OutlineRect(cx - GS!(7), cy - GS!(3), GS!(8), GS!(8), GS!(1));
						g.OutlineRect(cx - GS!(3), cy - GS!(7), GS!(8), GS!(8), GS!(1));
					}
					else
					{
						g.OutlineRect(cx - GS!(5), cy - GS!(5), GS!(10), GS!(10), GS!(1));
					}
				case .Close:
					g.DrawLine(cx - GS!(5), cy - GS!(5), cx + GS!(5), cy + GS!(5), GS!(1));
					g.DrawLine(cx - GS!(5), cy + GS!(5), cx + GS!(5), cy - GS!(5), GS!(1));
				}
			}
		}
	}


	public append List<Button> mButtons = .(8);
	public UISysMenuRoot mSysMenuRoot = new .() ~ delete _;
	public List<WindowButton> mWindowButtons = new .() ~ delete _;
	bool mHitTestAdded;
	String mLastTitle ~ delete _;

	public this()
	{
		mSysMenuRoot.mRootMenuBar = this;

		var minimizeButton = new WindowButton(.Minimize);
		minimizeButton.mOnMouseClick.Add(new (evt) => { mWidgetWindow.Show(.Minimized); });
		AddWidget(minimizeButton);
		mWindowButtons.Add(minimizeButton);

		var maximizeButton = new WindowButton(.MaximizeRestore);
		maximizeButton.mOnMouseClick.Add(new (evt) =>
			{
				if ((mWidgetWindow.mShowKind == .Maximized) || (mWidgetWindow.mShowKind == .ShowMaximized))
					mWidgetWindow.Show(.Normal);
				else
					mWidgetWindow.Show(.Maximized);
			});
		AddWidget(maximizeButton);
		mWindowButtons.Add(maximizeButton);

		var closeButton = new WindowButton(.Close);
		closeButton.mOnMouseClick.Add(new (evt) => { mWidgetWindow.Close(); });
		AddWidget(closeButton);
		mWindowButtons.Add(closeButton);
	}

	public override void AddedToWindow()
	{
		base.AddedToWindow();

		if ((!mHitTestAdded) && (mWidgetWindow.mWindowFlags.HasFlag(.ClientDecorations)))
		{
			mWidgetWindow.mOnHitTest.Add(new => HitTest);
			mHitTestAdded = true;
		}
	}

	public override void InitChildren()
	{
		base.InitChildren();
		mSysMenuRoot.mWindow = mWidgetWindow;
	}

	public void AddItemButton(Button btn)
	{
		mButtons.Add(btn);
		AddWidget(btn);
	}

	public void ShowMenu(Button btn)
	{
		if (btn.mSysMenu.mChildren == null)
			return;

		let menu = new Menu();
		btn.mSysMenu.mOnMenuItemUpdate(btn.mSysMenu);
		PopulateMenu(menu, btn.mSysMenu);
		btn.mMenuWidget = DarkTheme.sDarkTheme.CreateMenuWidget(menu);
		btn.mMenuWidget.Init(btn, 0, mHeight);

		btn.mMenuWidget.mOnRemovedFromParent.Add(new (widget, prevParent, widgetWindow) =>
			{
				btn.mMenuWidget = null;
			});
		btn.mMenuWidget.SetFocus();
	}

	void PopulateMenu(Menu parentMenu, SysMenu sysMenu)
	{
		if (sysMenu.mChildren == null)
			return;

		String buffer = scope .();
		for (let item in sysMenu.mChildren)
		{
			if (item.mText == null)
			{
				parentMenu.AddItem(null);
				continue;
			}

			StringView displayText = item.mText;
			if (!String.IsNullOrEmpty(item.mHotKey))
			{
				buffer..Clear().AppendF($"{item.mText}|{item.mHotKey}");
				displayText = buffer;
			}

			item.mOnMenuItemUpdate(item);
			let menu = AddMenuItem(parentMenu, displayText, new (menu) => item.mOnMenuItemSelected(menu), !item.mEnabled, item.mCheckState > 0, item.mRadioCheck);
			PopulateMenu(menu, item);
		}
	}

	Menu AddMenuItem(Menu menu, StringView label, delegate void(Menu menu) action, bool disabled = false, bool isChecked = false, bool radio = false)
	{
		Menu item = menu.AddItem(label);
		item.SetDisabled(disabled);

		if (isChecked)
		{
			item.mIconImage = DarkTheme.sDarkTheme.GetImage((radio ? .RadioOff : .Check));
		}

		if (action != null)
		{
			item.mOnMenuItemSelected.Add(action);
		}

		return item;
	}

	void ResizeComponents()
	{
		let font = DarkTheme.sDarkTheme.mSmallFont;
		let padding = GS!(20);
		float offset = 0;

		for (let btn in mButtons)
		{
			let width = font.GetWidth(btn.Label) + padding;
			btn.Resize(offset, 0, width, mHeight);
			offset += width;
		}

		// Window controls (minimize / maximize-restore / close) on the right
		float btnWidth = GS!(46);
		int32 buttonCount = (int32)mWindowButtons.Count;
		for (int32 i = 0; i < buttonCount; i++)
		{
			float x = mWidth - btnWidth * (buttonCount - i);
			mWindowButtons[i].Resize(x, 0, btnWidth, mHeight);
		}
	}

	public override void Resize(float x, float y, float width, float height)
	{
		base.Resize(x, y, width, height);
		ResizeComponents();
	}

	BFWindowBase.HitTestResult HitTest(int32 x, int32 y)
	{
		let window = mWidgetWindow;
		if (window == null)
			return .NotHandled;

		float lx = x - window.mClientX - mX;
		float ly = y - window.mClientY - mY;

		if ((lx >= 0) && (lx < mWidth) && (ly >= 0) && (ly < mHeight))
		{
			// Menu buttons and window controls are interactive; everything else in the strip
			// acts as the caption so the OS drags / double-click-maximizes natively.
			for (let btn in mButtons)
				if ((lx >= btn.mX) && (lx < btn.mX + btn.mWidth) && (ly >= btn.mY) && (ly < btn.mY + btn.mHeight))
					return .NotHandled;
			for (let windowButton in mWindowButtons)
				if ((lx >= windowButton.mX) && (lx < windowButton.mX + windowButton.mWidth) && (ly >= windowButton.mY) && (ly < windowButton.mY + windowButton.mHeight))
					return .NotHandled;

			return .Caption;
		}

		return .NotHandled;
	}

	public override void Update()
	{
		base.Update();

		// The title text is only updated through SetTitle, which doesn't mark us dirty, so track it
		if ((mLastTitle == null) || (mLastTitle != mWidgetWindow.mTitle))
		{
			String.NewOrSet!(mLastTitle, mWidgetWindow.mTitle);
			MarkDirty();
		}
	}

	public override void Draw(Graphics g)
	{
		using (g.PushColor(DarkTheme.COLOR_BKG))
		{
			g.FillRect(0, 0, mWidth, mHeight);
		}

		// Window title, centered in the space the menu buttons and window controls don't use
		let title = mWidgetWindow.mTitle;
		if (!String.IsNullOrEmpty(title))
		{
			float rightInset = mWindowButtons.Count * GS!(46);
			float leftInset = 0;
			for (let btn in mButtons)
				leftInset = Math.Max(leftInset, btn.mX + btn.mWidth);
			leftInset += GS!(8);

			float titleWidth = mWidth - leftInset - rightInset - GS!(8);
			if (titleWidth > GS!(20))
			{
				g.SetFont(DarkTheme.sDarkTheme.mSmallFont);
				uint32 titleColor = mWidgetWindow.mHasFocus ? DarkTheme.COLOR_TEXT : DarkTheme.COLOR_TEXT_DISABLED;
				using (g.PushColor(titleColor))
					g.DrawString(title, leftInset, (mHeight - GS!(20)) / 2, .Centered, titleWidth, .Ellipsis);
			}
		}
	}
}
