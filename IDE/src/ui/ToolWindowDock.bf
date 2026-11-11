using System;
using System.Collections;
using Beefy;
using Beefy.gfx;
using Beefy.geom;
using Beefy.theme;
using Beefy.theme.dark;
using Beefy.utils;
using Beefy.widgets;

namespace IDE.ui;

/*

Rider-style tool window docking.

The main window is divided into four tool "corners":

	+---------------------------+---------------------------+
	| [TL][TR] tool strips are on the left and right window |
	|      edges. Each strip has a top cluster and a bottom |
	|      cluster of tool icons.                           |
	+-------------------------------------------------------+

  TopLeft      - a tool pane docked to the left side, full height
  TopRight     - a tool pane docked to the right side, full height
  BottomLeft   - a tool pane docked under the document area (left half)
  BottomRight  - a tool pane docked under the document area (right half)

Clicking a tool icon toggles that tool. Only one tool per corner may be open at a
time - opening another tool in the same corner hides the currently open one.

The tool panes can be resized with the splitters between them and the document
area, but they can not be dragged/undocked - only the tool *icon* can be dragged,
which moves that tool to another corner.

*/

public enum ToolWindowCorner
{
	TopLeft,
	TopRight,
	BottomLeft,
	BottomRight,
	None
}

public enum ToolWindowIcon
{
	Workspace,
	ClassView,
	Output,
	Terminal,
	Console,
	Immediate,
	FindResults,
	AutoWatch,
	Watch,
	Memory,
	CallStack,
	Errors,
	Breakpoints,
	Diagnostics,
	Modules,
	Threads,
	Profile,
	Properties,
	Bookmarks
}

public class ToolWindow
{
	public String mName ~ delete _;
	public String mLabel ~ delete _;
	public ToolWindowIcon mIcon;
	public ToolWindowCorner mCorner;
	public Panel mPanel;
	public bool mIsOpen;

	public this(StringView name, StringView label, ToolWindowIcon icon, ToolWindowCorner corner, Panel panel)
	{
		mName = new String(name);
		mLabel = new String(label);
		mIcon = icon;
		mCorner = corner;
		mPanel = panel;
	}
}

public enum ToolDockSplitterKind
{
	None,
	LeftWidth,
	RightWidth,
	BottomHeight,
	BottomSplit
}

public class ToolWindowDock : Widget
{
	public DarkDockingFrame mDockingFrame;

	public ToolWindowStrip mLeftStrip;
	public ToolWindowStrip mRightStrip;

	public List<ToolWindow> mTools = new .() ~ DeleteContainerAndItems!(_);

	ToolWindowPane mTopLeftPane;
	ToolWindowPane mTopRightPane;
	ToolWindowPane mBottomLeftPane;
	ToolWindowPane mBottomRightPane;

	ToolDockSplitter mLeftSplitter;
	ToolDockSplitter mRightSplitter;
	ToolDockSplitter mBottomSplitter;
	ToolDockSplitter mBottomCenterSplitter;

	// All sizes are in unscaled (logical) units and are converted at layout time
	public float mTopLeftSize = 250;
	public float mTopRightSize = 250;
	public float mBottomSize = 250;
	public float mBottomSplit = 0.5f;

	// Current bottom area rect, used by the bottom-center splitter
	float mBottomRectX;
	float mBottomRectW;

	public ToolWindow mDraggingTool;
	public ToolWindowCorner mDropCorner = .None;

	// Tool icon moves are applied on the next update so we never delete the dragged icon widget
	//  while its own mouse event handlers are still running
	ToolWindow mPendingMoveTool;
	ToolWindowCorner mPendingMoveCorner = .None;

	public this()
	{
		mDockingFrame = (DarkDockingFrame)ThemeFactory.mDefault.CreateDockingFrame();
		AddWidget(mDockingFrame);

		mLeftStrip = new ToolWindowStrip(this, true);
		AddWidget(mLeftStrip);
		mRightStrip = new ToolWindowStrip(this, false);
		AddWidget(mRightStrip);

		mTopLeftPane = new ToolWindowPane(this, .TopLeft);
		AddWidget(mTopLeftPane);
		mTopRightPane = new ToolWindowPane(this, .TopRight);
		AddWidget(mTopRightPane);
		mBottomLeftPane = new ToolWindowPane(this, .BottomLeft);
		AddWidget(mBottomLeftPane);
		mBottomRightPane = new ToolWindowPane(this, .BottomRight);
		AddWidget(mBottomRightPane);

		mLeftSplitter = new ToolDockSplitter(this, .LeftWidth);
		AddWidget(mLeftSplitter);
		mRightSplitter = new ToolDockSplitter(this, .RightWidth);
		AddWidget(mRightSplitter);
		mBottomSplitter = new ToolDockSplitter(this, .BottomHeight);
		AddWidget(mBottomSplitter);
		mBottomCenterSplitter = new ToolDockSplitter(this, .BottomSplit);
		AddWidget(mBottomCenterSplitter);

		mTopLeftPane.SetVisible(false);
		mTopRightPane.SetVisible(false);
		mBottomLeftPane.SetVisible(false);
		mBottomRightPane.SetVisible(false);
		mLeftSplitter.SetVisible(false);
		mRightSplitter.SetVisible(false);
		mBottomSplitter.SetVisible(false);
		mBottomCenterSplitter.SetVisible(false);
	}

	public ~this()
	{
		// Tool panels are owned by IDEApp - make sure they don't get deleted with our panes
		for (var tool in mTools)
		{
			if ((tool.mPanel != null) && (tool.mPanel.mParent != null))
			{
				tool.mPanel.RemoveSelf();
				tool.mIsOpen = false;
			}
		}
	}

	public float GetStripWidth()
	{
		return GS!(26);
	}

	float GetSplitterSize()
	{
		return GS!(5);
	}

	ToolWindowPane GetPane(ToolWindowCorner corner)
	{
		switch (corner)
		{
		case .TopLeft:
			return mTopLeftPane;
		case .TopRight:
			return mTopRightPane;
		case .BottomLeft:
			return mBottomLeftPane;
		case .BottomRight:
			return mBottomRightPane;
		default:
			return null;
		}
	}

	ToolDockSplitter GetSplitter(ToolDockSplitterKind kind)
	{
		switch (kind)
		{
		case .LeftWidth:
			return mLeftSplitter;
		case .RightWidth:
			return mRightSplitter;
		case .BottomHeight:
			return mBottomSplitter;
		case .BottomSplit:
			return mBottomCenterSplitter;
		default:
			return null;
		}
	}

	public ToolWindow RegisterTool(StringView name, StringView label, ToolWindowIcon icon, ToolWindowCorner corner, Panel panel)
	{
		var tool = new ToolWindow(name, label, icon, corner, panel);
		mTools.Add(tool);

		bool wasOpen = (panel != null) && (panel.mParent != null);

		mLeftStrip.Refresh();
		mRightStrip.Refresh();
		Layout();

		if (wasOpen)
			OpenTool(tool);

		return tool;
	}

	public ToolWindow FindTool(StringView name)
	{
		for (var tool in mTools)
		{
			if (tool.mName == name)
				return tool;
		}
		return null;
	}

	public ToolWindow FindTool(Widget panel)
	{
		if (panel == null)
			return null;

		for (var tool in mTools)
		{
			if (tool.mPanel == panel)
				return tool;
		}
		return null;
	}

	public bool IsToolPanel(Widget panel)
	{
		return FindTool(panel) != null;
	}

	public bool IsToolOpen(Widget panel)
	{
		var tool = FindTool(panel);
		return (tool != null) && (tool.mIsOpen);
	}

	/// Opens the tool which owns the given panel (if any). Returns true if the panel is a tool panel.
	public bool ShowToolPanel(Widget panel)
	{
		var tool = FindTool(panel);
		if (tool == null)
			return false;

		if (!tool.mIsOpen)
			OpenTool(tool);
		return true;
	}

	public bool ToggleTool(ToolWindow tool)
	{
		if (tool == null)
			return false;

		if (tool.mIsOpen)
		{
			CloseTool(tool);
			return false;
		}
		else
		{
			OpenTool(tool);
			tool.mPanel?.FocusForKeyboard();
			return true;
		}
	}

	public void OpenTool(ToolWindow tool)
	{
		if ((tool == null) || (tool.mIsOpen))
			return;

		// Only one tool may be open per corner
		for (var otherTool in mTools)
		{
			if ((otherTool != tool) && (otherTool.mIsOpen) && (otherTool.mCorner == tool.mCorner))
			{
				CloseTool(otherTool);
				break;
			}
		}

		var pane = GetPane(tool.mCorner);
		if (pane == null)
			return;

		pane.SetTool(tool);
		tool.mIsOpen = true;
		Layout();
	}

	public void CloseTool(ToolWindow tool)
	{
		if ((tool == null) || (!tool.mIsOpen))
			return;

		var pane = GetPane(tool.mCorner);
		if (pane != null)
			pane.ClearTool();
		tool.mIsOpen = false;
		Layout();
	}

	public void CloseAllTools()
	{
		for (var tool in mTools)
		{
			if (tool.mIsOpen)
			{
				var pane = GetPane(tool.mCorner);
				if (pane != null)
					pane.ClearTool();
				tool.mIsOpen = false;
			}
		}
		Layout();
	}

	/// Moves a tool to another corner. If the tool is open then it is reopened in the new corner.
	public void MoveToolToCorner(ToolWindow tool, ToolWindowCorner corner)
	{
		if ((tool == null) || (corner == .None) || (tool.mCorner == corner))
			return;

		bool wasOpen = tool.mIsOpen;
		if (wasOpen)
			CloseTool(tool);

		tool.mCorner = corner;
		mLeftStrip.Refresh();
		mRightStrip.Refresh();

		if (wasOpen)
			OpenTool(tool);

		Layout();
	}

	/// Recreates the document area docking frame (used by Reset UI)
	public void ResetDocumentArea()
	{
		mDockingFrame.RemoveSelf();
		BFApp.sApp.DeferDelete(mDockingFrame);
		mDockingFrame = (DarkDockingFrame)ThemeFactory.mDefault.CreateDockingFrame();
		AddWidgetAtIndex(0, mDockingFrame);
		Layout();
	}

	public override void Resize(float x, float y, float width, float height)
	{
		base.Resize(x, y, width, height);
		Layout();
	}

	public void Layout()
	{
		float width = mWidth;
		float height = mHeight;
		if ((width <= 0) || (height <= 0))
			return;

		float scale = DarkTheme.sScale;
		float stripWidth = GetStripWidth();
		float splitterSize = GetSplitterSize();

		bool showLeftStrip = (mLeftStrip.mButtons.Count > 0) || (mDraggingTool != null);
		bool showRightStrip = (mRightStrip.mButtons.Count > 0) || (mDraggingTool != null);

		float leftStripWidth = showLeftStrip ? stripWidth : 0;
		float rightStripWidth = showRightStrip ? stripWidth : 0;

		if (showLeftStrip)
			mLeftStrip.Resize(0, 0, leftStripWidth, height);
		mLeftStrip.SetVisible(showLeftStrip);

		if (showRightStrip)
			mRightStrip.Resize(width - rightStripWidth, 0, rightStripWidth, height);
		mRightStrip.SetVisible(showRightStrip);

		float contentLeft = leftStripWidth;
		float contentRight = width - rightStripWidth;
		float contentBottom = height;

		// Left (TopLeft) pane - spans the full height
		if (mTopLeftPane.mTool != null)
		{
			float paneWidth = Math.Clamp((float)Math.Round(mTopLeftSize * scale), GS!(120), Math.Max(GS!(120), width * 0.45f));
			mTopLeftPane.Resize(contentLeft, 0, paneWidth, height);
			mTopLeftPane.SetVisible(true);
			contentLeft += paneWidth;

			mLeftSplitter.Resize(contentLeft, 0, splitterSize, height);
			mLeftSplitter.SetVisible(true);
			contentLeft += splitterSize;
		}
		else
		{
			mTopLeftPane.SetVisible(false);
			mLeftSplitter.SetVisible(false);
		}

		// Right (TopRight) pane - spans the full height
		if (mTopRightPane.mTool != null)
		{
			float paneWidth = Math.Clamp((float)Math.Round(mTopRightSize * scale), GS!(120), Math.Max(GS!(120), width * 0.45f));
			float paneX = contentRight - paneWidth;
			mTopRightPane.Resize(paneX, 0, paneWidth, height);
			mTopRightPane.SetVisible(true);

			mRightSplitter.Resize(paneX - splitterSize, 0, splitterSize, height);
			mRightSplitter.SetVisible(true);
			contentRight = paneX - splitterSize;
		}
		else
		{
			mTopRightPane.SetVisible(false);
			mRightSplitter.SetVisible(false);
		}

		// Bottom panes
		bool bottomLeftOpen = mBottomLeftPane.mTool != null;
		bool bottomRightOpen = mBottomRightPane.mTool != null;

		if (bottomLeftOpen || bottomRightOpen)
		{
			float bottomHeight = Math.Clamp((float)Math.Round(mBottomSize * scale), GS!(80), Math.Max(GS!(80), height * 0.5f));
			float bottomY = height - bottomHeight;
			float bottomWidth = Math.Max(0, contentRight - contentLeft);

			mBottomSplitter.Resize(contentLeft, bottomY - splitterSize, bottomWidth, splitterSize);
			mBottomSplitter.SetVisible(true);
			contentBottom = bottomY - splitterSize;

			mBottomRectX = contentLeft;
			mBottomRectW = bottomWidth;

			if ((bottomLeftOpen) && (bottomRightOpen))
			{
				float leftWidth = (float)Math.Round(Math.Clamp(mBottomSplit, 0.15f, 0.85f) * bottomWidth);
				float rightWidth = Math.Max(0, bottomWidth - leftWidth - splitterSize);

				mBottomLeftPane.Resize(contentLeft, bottomY, leftWidth, bottomHeight);
				mBottomLeftPane.SetVisible(true);

				mBottomCenterSplitter.Resize(contentLeft + leftWidth, bottomY, splitterSize, bottomHeight);
				mBottomCenterSplitter.SetVisible(true);

				mBottomRightPane.Resize(contentLeft + leftWidth + splitterSize, bottomY, rightWidth, bottomHeight);
				mBottomRightPane.SetVisible(true);
			}
			else if (bottomLeftOpen)
			{
				mBottomLeftPane.Resize(contentLeft, bottomY, bottomWidth, bottomHeight);
				mBottomLeftPane.SetVisible(true);
				mBottomRightPane.SetVisible(false);
				mBottomCenterSplitter.SetVisible(false);
			}
			else
			{
				mBottomRightPane.Resize(contentLeft, bottomY, bottomWidth, bottomHeight);
				mBottomRightPane.SetVisible(true);
				mBottomLeftPane.SetVisible(false);
				mBottomCenterSplitter.SetVisible(false);
			}
		}
		else
		{
			mBottomLeftPane.SetVisible(false);
			mBottomRightPane.SetVisible(false);
			mBottomSplitter.SetVisible(false);
			mBottomCenterSplitter.SetVisible(false);
		}

		mDockingFrame.Resize(contentLeft, 0, Math.Max(0, contentRight - contentLeft), Math.Max(0, contentBottom));
	}

	public void DragSplitter(ToolDockSplitterKind kind, float x, float y)
	{
		float scale = DarkTheme.sScale;
		float stripWidth = GetStripWidth();

		if (kind == .LeftWidth)
		{
			float maxSize = Math.Max(120.0f, (mWidth * 0.45f) / scale);
			mTopLeftSize = Math.Clamp((x - stripWidth) / scale, 120.0f, maxSize);
		}
		else if (kind == .RightWidth)
		{
			float maxSize = Math.Max(120.0f, (mWidth * 0.45f) / scale);
			mTopRightSize = Math.Clamp((mWidth - stripWidth - x) / scale, 120.0f, maxSize);
		}
		else if (kind == .BottomHeight)
		{
			float maxSize = Math.Max(80.0f, (mHeight * 0.5f) / scale);
			mBottomSize = Math.Clamp((mHeight - y) / scale, 80.0f, maxSize);
		}
		else if (kind == .BottomSplit)
		{
			float split = (x - mBottomRectX) / Math.Max(1.0f, mBottomRectW);
			mBottomSplit = Math.Clamp(split, 0.15f, 0.85f);
		}

		Layout();
	}

	public void BeginToolDrag(ToolWindow tool)
	{
		mDraggingTool = tool;
		mDropCorner = .None;

		// Both strips need to be visible while dragging so any corner can be a drop target
		Layout();
		MarkDirty();
	}

	public void UpdateToolDrag(float rootX, float rootY)
	{
		float x;
		float y;
		RootToSelfTranslate(rootX, rootY, out x, out y);

		ToolWindowCorner corner = GetDropCorner(x, y);
		if (corner != mDropCorner)
		{
			mDropCorner = corner;
			MarkDirty();
		}
	}

	public void EndToolDrag()
	{
		var dragTool = mDraggingTool;
		var dropCorner = mDropCorner;

		mDraggingTool = null;
		mDropCorner = .None;

		if ((dragTool != null) && (dropCorner != .None) && (dropCorner != dragTool.mCorner))
		{
			mPendingMoveTool = dragTool;
			mPendingMoveCorner = dropCorner;
		}

		Layout();
		MarkDirty();
	}

	public override void Update()
	{
		base.Update();

		if (mPendingMoveTool != null)
		{
			var moveTool = mPendingMoveTool;
			var moveCorner = mPendingMoveCorner;
			mPendingMoveTool = null;
			mPendingMoveCorner = .None;
			MoveToolToCorner(moveTool, moveCorner);
		}
	}

	public override void Draw(Graphics g)
	{
		using (g.PushColor(DarkTheme.COLOR_BKG))
			g.FillRect(0, 0, mWidth, mHeight);
	}

	public ToolWindowCorner GetDropCorner(float x, float y)
	{
		if ((mWidth <= 0) || (mHeight <= 0))
			return .None;

		float stripWidth = GetStripWidth();

		if ((x < stripWidth) && (mLeftStrip.mVisible))
			return (y < mHeight * 0.5f) ? .TopLeft : .BottomLeft;

		if ((x >= mWidth - stripWidth) && (mRightStrip.mVisible))
			return (y < mHeight * 0.5f) ? .TopRight : .BottomRight;

		// Allow dropping onto an already-visible pane as well
		if ((mTopLeftPane.mTool != null) && (mTopLeftPane.mVisible) && (ContainsPoint(mTopLeftPane, x, y)))
			return .TopLeft;
		if ((mTopRightPane.mTool != null) && (mTopRightPane.mVisible) && (ContainsPoint(mTopRightPane, x, y)))
			return .TopRight;
		if ((mBottomLeftPane.mTool != null) && (mBottomLeftPane.mVisible) && (ContainsPoint(mBottomLeftPane, x, y)))
			return .BottomLeft;
		if ((mBottomRightPane.mTool != null) && (mBottomRightPane.mVisible) && (ContainsPoint(mBottomRightPane, x, y)))
			return .BottomRight;

		return .None;
	}

	static bool ContainsPoint(Widget widget, float x, float y)
	{
		return (x >= widget.mX) && (y >= widget.mY) &&
			(x < widget.mX + widget.mWidth) && (y < widget.mY + widget.mHeight);
	}

	public override void DrawAll(Graphics g)
	{
		base.DrawAll(g);

		// Draw the dragged tool icon last so it is above everything else
		if ((mDraggingTool != null) && (mWidgetWindow != null))
		{
			float x;
			float y;
			RootToSelfTranslate(mWidgetWindow.mMouseX, mWidgetWindow.mMouseY, out x, out y);

			float iconSize = GS!(16);
			using (g.PushColor(0xC0202028)) // @Todo: Proper color
				g.FillRect(x - iconSize * 0.5f - GS!(4), y - iconSize * 0.5f - GS!(4), iconSize + GS!(8), iconSize + GS!(8));
			using (g.PushColor(0xFFFFFFFF)) // @Todo: Proper color
				g.OutlineRect(x - iconSize * 0.5f - GS!(4), y - iconSize * 0.5f - GS!(4), iconSize + GS!(8), iconSize + GS!(8), GS!(1));
			DrawIcon(g, mDraggingTool.mIcon, x - iconSize * 0.5f, y - iconSize * 0.5f, iconSize, 0xFFFFFFFF);
		}
	}

	public override void RehupScale(float oldScale, float newScale)
	{
		base.RehupScale(oldScale, newScale);

		// Panels which are currently closed are not in our widget tree, so scale them manually
		for (var tool in mTools)
		{
			if ((tool.mPanel != null) && (tool.mPanel.mWidgetWindow == null))
				tool.mPanel.RehupScale(oldScale, newScale);
		}
	}

	public void ClearPanels()
	{
		CloseAllTools();
		for (var tool in mTools)
			tool.mPanel = null;
	}

	public void SerializeLayout(StructuredData data)
	{
		data.Add("TopLeftSize", mTopLeftSize);
		data.Add("TopRightSize", mTopRightSize);
		data.Add("BottomSize", mBottomSize);
		data.Add("BottomSplit", mBottomSplit);

		using (data.CreateArray("Tools"))
		{
			for (var tool in mTools)
			{
				using (data.CreateObject())
				{
					data.Add("Name", tool.mName);
					data.Add("Corner", (int32)tool.mCorner);
					if (tool.mIsOpen)
						data.Add("Open", true);

					if ((tool.mPanel != null) && (tool.mPanel.WantsSerialization))
						tool.mPanel.Serialize(data);
				}
			}
		}
	}

	public bool DeserializeLayout(StructuredData data)
	{
		// Older layouts (and debug session files) have no tool window section at all - leave
		//  the current layout alone in that case
		if (data.Contains("TopLeftSize"))
			mTopLeftSize = data.GetFloat("TopLeftSize", mTopLeftSize);
		if (data.Contains("TopRightSize"))
			mTopRightSize = data.GetFloat("TopRightSize", mTopRightSize);
		if (data.Contains("BottomSize"))
			mBottomSize = data.GetFloat("BottomSize", mBottomSize);
		if (data.Contains("BottomSplit"))
			mBottomSplit = data.GetFloat("BottomSplit", mBottomSplit);

		for (var _toolEntry in data.Enumerate("Tools"))
		{
			String toolName = scope String();
			data.GetString("Name", toolName);

			var tool = FindTool(toolName);
			if (tool == null)
				continue;

			tool.mCorner = (ToolWindowCorner)data.GetInt("Corner", (int32)tool.mCorner);

			if ((tool.mPanel != null) && (tool.mPanel.WantsSerialization))
				tool.mPanel.Deserialize(data);

			if (data.GetBool("Open"))
				OpenTool(tool);
		}

		mLeftStrip.Refresh();
		mRightStrip.Refresh();
		Layout();
		return true;
	}

	// @Todo: This entire thing needs to go..
	public static void DrawIcon(Graphics g, ToolWindowIcon icon, float x, float y, float size, Color color)
	{
		using (g.PushColor(color))
		{
			float u = size / 20.0f;

			switch (icon)
			{
			case .Workspace:
				{
					// Folder
					g.FillRect(x + 2 * u, y + 4 * u, 7 * u, 3 * u);
					g.OutlineRect(x + 2 * u, y + 6 * u, 16 * u, 10 * u, 1.5f * u);
				}
			case .ClassView:
				{
					// Hierarchy
					g.FillRect(x + 2 * u, y + 3 * u, 6 * u, 5 * u);
					g.FillRect(x + 12 * u, y + 2 * u, 6 * u, 5 * u);
					g.FillRect(x + 12 * u, y + 13 * u, 6 * u, 5 * u);
					g.DrawLine(x + 8 * u, y + 5.5f * u, x + 12 * u, y + 4.5f * u, 1.5f * u);
					g.DrawLine(x + 8 * u, y + 5.5f * u, x + 12 * u, y + 15.5f * u, 1.5f * u);
				}
			case .Output:
				{
					// Text lines
					g.FillRect(x + 3 * u, y + 5 * u, 14 * u, 2 * u);
					g.FillRect(x + 3 * u, y + 9 * u, 10 * u, 2 * u);
					g.FillRect(x + 3 * u, y + 13 * u, 12 * u, 2 * u);
				}
			case .Terminal:
				{
					// Prompt in a box
					g.OutlineRect(x + 2 * u, y + 3 * u, 16 * u, 14 * u, 1.5f * u);
					g.DrawLine(x + 5 * u, y + 7 * u, x + 8 * u, y + 10 * u, 1.5f * u);
					g.DrawLine(x + 8 * u, y + 10 * u, x + 5 * u, y + 13 * u, 1.5f * u);
					g.FillRect(x + 10 * u, y + 12 * u, 5 * u, 1.5f * u);
				}
			case .Console:
				{
					// Window with text
					g.OutlineRect(x + 2 * u, y + 3 * u, 16 * u, 14 * u, 1.5f * u);
					g.FillRect(x + 5 * u, y + 7 * u, 10 * u, 1.5f * u);
					g.FillRect(x + 5 * u, y + 10 * u, 10 * u, 1.5f * u);
					g.FillRect(x + 5 * u, y + 13 * u, 6 * u, 1.5f * u);
				}
			case .Immediate:
				{
					// Prompt line
					g.DrawLine(x + 4 * u, y + 6 * u, x + 8 * u, y + 10 * u, 1.5f * u);
					g.DrawLine(x + 8 * u, y + 10 * u, x + 4 * u, y + 14 * u, 1.5f * u);
					g.FillRect(x + 10 * u, y + 12.5f * u, 7 * u, 1.5f * u);
				}
			case .FindResults:
				{
					// Magnifier
					g.OutlineCircle(x + 8.5f * u, y + 8.5f * u, 5 * u);
					g.DrawLine(x + 12.5f * u, y + 12.5f * u, x + 17 * u, y + 17 * u, 2 * u);
				}
			case .AutoWatch:
				{
					// Eye with a plus
					g.OutlineOval(x + 9 * u, y + 11 * u, 6 * u, 4 * u);
					g.FillCircle(x + 9 * u, y + 11 * u, 2 * u);
					g.DrawLine(x + 16 * u, y + 2 * u, x + 16 * u, y + 7 * u, 1.5f * u);
					g.DrawLine(x + 13.5f * u, y + 4.5f * u, x + 18.5f * u, y + 4.5f * u, 1.5f * u);
				}
			case .Watch:
				{
					// Eye
					g.OutlineOval(x + 10 * u, y + 10 * u, 7 * u, 4.5f * u);
					g.FillCircle(x + 10 * u, y + 10 * u, 2.5f * u);
				}
			case .Memory:
				{
					// Memory chip
					g.OutlineRect(x + 4 * u, y + 4 * u, 12 * u, 12 * u, 1.5f * u);
					g.FillRect(x + 1.5f * u, y + 6 * u, 2.5f * u, 2.5f * u);
					g.FillRect(x + 1.5f * u, y + 11.5f * u, 2.5f * u, 2.5f * u);
					g.FillRect(x + 16 * u, y + 6 * u, 2.5f * u, 2.5f * u);
					g.FillRect(x + 16 * u, y + 11.5f * u, 2.5f * u, 2.5f * u);
				}
			case .CallStack:
				{
					// Stacked frames
					g.FillRect(x + 3 * u, y + 4 * u, 14 * u, 3 * u);
					g.FillRect(x + 3 * u, y + 9 * u, 14 * u, 3 * u);
					g.FillRect(x + 3 * u, y + 14 * u, 14 * u, 3 * u);
				}
			case .Errors:
				{
					// Circle with an exclamation mark
					g.OutlineCircle(x + 10 * u, y + 10 * u, 7 * u);
					g.FillRect(x + 9.25f * u, y + 5.5f * u, 1.5f * u, 6.5f * u);
					g.FillRect(x + 9.25f * u, y + 14 * u, 1.5f * u, 1.5f * u);
				}
			case .Breakpoints:
				{
					// Ring
					g.OutlineCircle(x + 10 * u, y + 10 * u, 7 * u);
					g.FillCircle(x + 10 * u, y + 10 * u, 3 * u);
				}
			case .Diagnostics:
				{
					// Pulse
					g.DrawLine(x + 1.5f * u, y + 11 * u, x + 5 * u, y + 11 * u, 1.5f * u);
					g.DrawLine(x + 5 * u, y + 11 * u, x + 8 * u, y + 4 * u, 1.5f * u);
					g.DrawLine(x + 8 * u, y + 4 * u, x + 12 * u, y + 16 * u, 1.5f * u);
					g.DrawLine(x + 12 * u, y + 16 * u, x + 14 * u, y + 11 * u, 1.5f * u);
					g.DrawLine(x + 14 * u, y + 11 * u, x + 18.5f * u, y + 11 * u, 1.5f * u);
				}
			case .Modules:
				{
					// Box
					g.OutlineRect(x + 3 * u, y + 7 * u, 14 * u, 10 * u, 1.5f * u);
					g.DrawLine(x + 3 * u, y + 7 * u, x + 10 * u, y + 2.5f * u, 1.5f * u);
					g.DrawLine(x + 17 * u, y + 7 * u, x + 10 * u, y + 2.5f * u, 1.5f * u);
				}
			case .Threads:
				{
					// Bulleted list
					g.FillCircle(x + 4 * u, y + 6 * u, 1.5f * u);
					g.FillCircle(x + 4 * u, y + 10 * u, 1.5f * u);
					g.FillCircle(x + 4 * u, y + 14 * u, 1.5f * u);
					g.FillRect(x + 8 * u, y + 5 * u, 9 * u, 2 * u);
					g.FillRect(x + 8 * u, y + 9 * u, 9 * u, 2 * u);
					g.FillRect(x + 8 * u, y + 13 * u, 9 * u, 2 * u);
				}
			case .Profile:
				{
					// Bar chart
					g.FillRect(x + 3 * u, y + 12 * u, 4 * u, 5 * u);
					g.FillRect(x + 8 * u, y + 8 * u, 4 * u, 9 * u);
					g.FillRect(x + 13 * u, y + 4 * u, 4 * u, 13 * u);
				}
			case .Properties:
				{
					// Sliders
					g.FillRect(x + 2 * u, y + 6 * u, 16 * u, 1.5f * u);
					g.FillCircle(x + 7 * u, y + 6.75f * u, 2 * u);
					g.FillRect(x + 2 * u, y + 13 * u, 16 * u, 1.5f * u);
					g.FillCircle(x + 13 * u, y + 13.75f * u, 2 * u);
				}
			case .Bookmarks:
				{
					// Bookmark
					g.OutlineRect(x + 5 * u, y + 2.5f * u, 10 * u, 14.5f * u, 1.5f * u);
					g.DrawLine(x + 5 * u, y + 17 * u, x + 10 * u, y + 12 * u, 1.5f * u);
					g.DrawLine(x + 10 * u, y + 12 * u, x + 15 * u, y + 17 * u, 1.5f * u);
				}
			}
		}
	}
}

/// The vertical icon strip on the left or right edge of the window
public class ToolWindowStrip : Widget
{
	public ToolWindowDock mDock;
	public bool mIsLeft;
	public List<ToolIconButton> mButtons = new .() ~ delete _;

	public this(ToolWindowDock dock, bool isLeft)
	{
		mDock = dock;
		mIsLeft = isLeft;
	}

	public ~this()
	{
		for (var button in mButtons)
			Widget.RemoveAndDelete(button);
	}

	public bool ContainsCorner(ToolWindowCorner corner)
	{
		if (mIsLeft)
			return (corner == .TopLeft) || (corner == .BottomLeft);
		return (corner == .TopRight) || (corner == .BottomRight);
	}

	public void Refresh()
	{
		for (var button in mButtons)
			Widget.RemoveAndDelete(button);
		mButtons.Clear();

		for (var tool in mDock.mTools)
		{
			if (ContainsCorner(tool.mCorner))
			{
				var button = new ToolIconButton(mDock, tool);
				mButtons.Add(button);
				AddWidget(button);
			}
		}

		Resize(mX, mY, mWidth, mHeight);
	}

	public override void Resize(float x, float y, float width, float height)
	{
		base.Resize(x, y, width, height);

		float step = GS!(26);
		float curY = GS!(4);

		for (var button in mButtons)
		{
			if ((button.mTool.mCorner == .TopLeft) || (button.mTool.mCorner == .TopRight))
			{
				button.Resize(0, curY, width, step);
				curY += step;
			}
		}

		curY = height - GS!(4);
		for (int32 buttonIdx = (int32)mButtons.Count - 1; buttonIdx >= 0; buttonIdx--)
		{
			var button = mButtons[buttonIdx];
			if ((button.mTool.mCorner == .BottomLeft) || (button.mTool.mCorner == .BottomRight))
			{
				curY -= step;
				button.Resize(0, curY, width, step);
			}
		}
	}

	public override void Draw(Graphics g)
	{
		using (g.PushColor(DarkTheme.COLOR_BKG))
			g.FillRect(0, 0, mWidth, mHeight);


		// using (g.PushColor(DarkTheme.COLOR_TEXT_DISABLED))
		// {
		// 	if (mIsLeft)
		// 		g.FillRect(mWidth - GS!(1), 0, GS!(1), mHeight);
		// 	else
		// 		g.FillRect(0, 0, GS!(1), mHeight);
		// }

		// Highlight the corner the dragged icon would be dropped on
		if ((mDock.mDraggingTool != null) && (mDock.mDropCorner != .None) && (ContainsCorner(mDock.mDropCorner)))
		{
			bool isTopCorner = (mDock.mDropCorner == .TopLeft) || (mDock.mDropCorner == .TopRight);
			using (g.PushColor(0x30FFD080))
			{
				if (isTopCorner)
					g.FillRect(0, 0, mWidth, mHeight * 0.5f);
				else
					g.FillRect(0, mHeight * 0.5f, mWidth, mHeight * 0.5f);
			}
		}
	}
}

/// A single draggable tool icon inside a strip
public class ToolIconButton : Widget, IDragInterface
{
	public ToolWindowDock mDock;
	public ToolWindow mTool;
	public DragHelper mDragHelper ~ delete _;
	bool mDidDrag;

	public this(ToolWindowDock dock, ToolWindow tool)
	{
		mDock = dock;
		mTool = tool;
		mDragHelper = new DragHelper(this, this);
		mDragHelper.mTriggerDist = GS!(4);
	}

	public override void Draw(Graphics g)
	{
		if (mTool.mIsOpen)
		{
			let active = mMouseOver || mMouseDown;
			using (g.PushColor(active ? DarkTheme.COLOR_BUTTON_HOVER : DarkTheme.COLOR_BUTTON))
				g.FillRect(0, 0, mWidth, mHeight);
			using (g.PushColor(DarkTheme.COLOR_SELECTED_OUTLINE))
			{
				let corner = mTool.mCorner;
				let width = GS!(4);
				if (corner == .BottomRight || corner == .TopRight)
					g.FillRect(0, 0, width, mHeight);
				else
					g.FillRect(mWidth - width, 0, width, mHeight);
			}
		}
		else if (mMouseOver)
		{
			using (g.PushColor(0x22AAAAAA))
				g.FillRect(0, 0, mWidth, mHeight);
		}

		float iconSize = GS!(15);
		float iconX = (mWidth - iconSize) * 0.5f;
		float iconY = (mHeight - iconSize) * 0.5f;
		if (mTool.mIsOpen)
			ToolWindowDock.DrawIcon(g, mTool.mIcon, iconX, iconY, iconSize, DarkTheme.COLOR_TEXT);
		else
			ToolWindowDock.DrawIcon(g, mTool.mIcon, iconX, iconY, iconSize, DarkTheme.COLOR_TEXT_DISABLED);
	}

	public override void MouseDown(float x, float y, int32 btn, int32 btnCount)
	{
		mDidDrag = false;
		base.MouseDown(x, y, btn, btnCount);
	}

	public override void MouseUp(float x, float y, int32 btn)
	{
		base.MouseUp(x, y, btn);

		if ((btn == 0) && (!mDidDrag) && (mMouseOver))
			mDock.ToggleTool(mTool);
	}

	public void DragStart()
	{
		mDidDrag = true;
		mDock.BeginToolDrag(mTool);
	}

	public void MouseDrag(float x, float y, float dX, float dY)
	{
		float rootX;
		float rootY;
		SelfToRootTranslate(x, y, out rootX, out rootY);
		mDock.UpdateToolDrag(rootX, rootY);
	}

	public void DragEnd()
	{
		mDock.EndToolDrag();
	}

	public override void Update()
	{
		base.Update();

		if (DarkTooltipManager.sLastMouseWidget == this)
		{
			Point point;
			if (DarkTooltipManager.CheckMouseover(this, 20, out point))
				DarkTooltipManager.ShowTooltip(mTool.mLabel, this, 0, mHeight);
		}
	}
}

/// A resizable tool window pane. It has a header showing the tool title and a close button.
public class ToolWindowPane : Widget
{
	public ToolWindowDock mDock;
	public ToolWindowCorner mCorner;
	public ToolWindow mTool;

	ToolPaneCloseButton mCloseButton;
	public float mHeaderHeight = GS!(20);

	public this(ToolWindowDock dock, ToolWindowCorner corner)
	{
		mDock = dock;
		mCorner = corner;
		mClipGfx = true;
		mClipMouse = true;

		mCloseButton = new ToolPaneCloseButton(this);
		AddWidget(mCloseButton);
	}

	public void SetTool(ToolWindow tool)
	{
		if (mTool == tool)
			return;

		ClearTool();

		mTool = tool;
		if (tool.mPanel != null)
		{
			if (tool.mPanel.mParent != null)
				tool.mPanel.RemoveSelf();
			AddWidget(tool.mPanel);
		}

		ResizeContent();
	}

	public void ClearTool()
	{
		if (mTool == null)
			return;

		if ((mTool.mPanel != null) && (mTool.mPanel.mParent == this))
			mTool.mPanel.RemoveSelf();
		mTool = null;
	}

	void ResizeContent()
	{
		if ((mTool != null) && (mTool.mPanel != null) && (mTool.mPanel.mParent == this))
			mTool.mPanel.Resize(0, mHeaderHeight, mWidth, Math.Max(0, mHeight - mHeaderHeight));
	}

	public override void Resize(float x, float y, float width, float height)
	{
		base.Resize(x, y, width, height);

		if (mCloseButton != null)
			mCloseButton.Resize(mWidth - GS!(20), 0, GS!(20), Math.Min(mHeaderHeight, mHeight));

		ResizeContent();
	}

	public override void RehupScale(float oldScale, float newScale)
	{
		base.RehupScale(oldScale, newScale);
		mHeaderHeight = GS!(20);
		Resize(mX, mY, mWidth, mHeight);
	}

	public override void Draw(Graphics g)
	{
		using (g.PushColor(DarkTheme.COLOR_BKG_ALT))
			g.FillRect(0, 0, mWidth, mHeight);

		if (mTool == null)
			return;

		float headerHeight = Math.Min(mHeaderHeight, mHeight);

		g.SetFont(DarkTheme.sDarkTheme.mSmallFont);
		using (g.PushColor(DarkTheme.COLOR_TEXT))
			g.DrawString(mTool.mLabel, GS!(8), (headerHeight - g.mFont.GetLineSpacing()) * 0.5f, .Left, mWidth - GS!(30), .Ellipsis);

		using (g.PushColor(DarkTheme.COLOR_TEXT_DISABLED))
			g.FillRect(0, headerHeight - GS!(1), mWidth, GS!(1));
	}
}

class ToolPaneCloseButton : ButtonWidget
{
	public ToolWindowPane mPane;

	public this(ToolWindowPane pane)
	{
		mPane = pane;
	}

	public override void Draw(Graphics g)
	{
		if ((mMouseOver) || (mMouseDown))
		{
			using (g.PushColor(0x30FFFFFF))
				g.FillRect(0, 0, mWidth, mHeight);
		}

		Image image = mMouseOver ? DarkTheme.sDarkTheme.GetImage(DarkTheme.ImageIdx.CloseOver) : DarkTheme.sDarkTheme.GetImage(DarkTheme.ImageIdx.Close);
		g.Draw(image, (mWidth - image.mWidth) * 0.5f, (mHeight - image.mHeight) * 0.5f);
	}

	public override void MouseClicked(float x, float y, float origX, float origY, int32 btn)
	{
		base.MouseClicked(x, y, origX, origY, btn);

		if (btn == 0)
			mPane.mDock.CloseTool(mPane.mTool);
	}
}

/// A splitter between a tool pane and the document area
public class ToolDockSplitter : Widget
{
	public ToolWindowDock mDock;
	public ToolDockSplitterKind mKind;
	bool mVertical;

	public this(ToolWindowDock dock, ToolDockSplitterKind kind)
	{
		mDock = dock;
		mKind = kind;
		mVertical = (kind == .LeftWidth) || (kind == .RightWidth) || (kind == .BottomSplit);
	}

	public override void Draw(Graphics g)
	{
		if ((mMouseOver) || (mMouseDown))
		{
			using (g.PushColor(0x40FFFFFF))
				g.FillRect(0, 0, mWidth, mHeight);
		}
	}

	public override void MouseMove(float x, float y)
	{
		base.MouseMove(x, y);

		BFApp.sApp.SetCursor(mVertical ? Cursor.SizeWE : Cursor.SizeNS);

		if (mMouseDown)
			mDock.DragSplitter(mKind, mX + x, mY + y);
	}

	public override void MouseLeave()
	{
		base.MouseLeave();
		if (!mMouseDown)
			BFApp.sApp.SetCursor(Cursor.Pointer);
	}

	public override void MouseUp(float x, float y, int32 btn)
	{
		base.MouseUp(x, y, btn);
		BFApp.sApp.SetCursor(mVertical ? Cursor.SizeWE : Cursor.SizeNS);
	}
}
