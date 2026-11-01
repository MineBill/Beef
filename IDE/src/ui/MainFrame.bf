using System;
using System.Collections;
using System.Text;
using System.Threading.Tasks;
using Beefy;
using Beefy.gfx;
using Beefy.widgets;
using Beefy.theme;
using Beefy.theme.dark;

namespace IDE.ui
{
    public class MainFrame : Widget
    {
        public StatusBar mStatusBar;
        public DarkDockingFrame mDockingFrame;
        public MenuBar mMenuBar;

        public this()
        {
            mStatusBar = new StatusBar();
            AddWidget(mStatusBar);
            mDockingFrame = (DarkDockingFrame)ThemeFactory.mDefault.CreateDockingFrame();
            AddWidget(mDockingFrame);
            mMenuBar = new MenuBar();
            AddWidget(mMenuBar);
        }

        public void Reset()
        {
            mDockingFrame.RemoveSelf();
            gApp.DeferDelete(mDockingFrame);
            mDockingFrame = (DarkDockingFrame)ThemeFactory.mDefault.CreateDockingFrame();
            AddWidget(mDockingFrame);

            RehupSize();
        }

        public ~this()
        {
        }

        public override void Resize(float x, float y, float width, float height)
        {
            base.Resize(x, y, width, height);

            int32 statusHeight = GS!(20);
            int32 menuHeight = GS!(24);
            mDockingFrame.Resize(0, menuHeight, width, height - statusHeight - menuHeight);
            mMenuBar.Resize(0, 0, width, menuHeight);
            mStatusBar.Resize(0, mHeight - statusHeight, width, statusHeight);
        }
    }
}
