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
        public ToolWindowDock mToolDock;

        public this()
        {
            mStatusBar = new StatusBar();
            AddWidget(mStatusBar);

            // The tool dock owns the document area docking frame as its center
            mToolDock = new ToolWindowDock();
            AddWidget(mToolDock);
            mDockingFrame = mToolDock.mDockingFrame;

            mMenuBar = new MenuBar();
            AddWidget(mMenuBar);
        }

        public void Reset()
        {
            // The tool windows (and their corner assignments) survive a UI reset - only the
            //  document area docking frame gets recreated
            mToolDock.ResetDocumentArea();
            mDockingFrame = mToolDock.mDockingFrame;

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
            mToolDock.Resize(0, menuHeight, width, Math.Clamp(height - statusHeight - menuHeight, 0, float.MaxValue));
            mMenuBar.Resize(0, 0, width, menuHeight);
            mStatusBar.Resize(0, mHeight - statusHeight, width, statusHeight);
        }
    }
}
