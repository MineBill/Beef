using System;
using System.Collections;
using System.Text;
using Beefy.widgets;
using Beefy.gfx;
using Beefy.geom;

namespace Beefy.theme.dark
{
	public struct Padding
	{
		public float Left;
		public float Right;
		public float Top;
		public float Bottom;

		public this(float padding)
		{
			Left = padding;
			Right = padding;
			Top = padding;
			Bottom = padding;
		}

		public this(float leftRight, float topBottom)
		{
			Left = leftRight;
			Right = leftRight;
			Top = topBottom;
			Bottom = topBottom;
		}

		public this(float left, float right, float top, float bottom)
		{
			Left = left;
			Right = right;
			Top = top;
			Bottom = bottom;
		}
	}

    public class DarkIconButton : ButtonWidget
    {
		private Image mIcon;

		private Padding mPadding = .(4);

		public Image Icon
		{
			get => mIcon;
			set
			{
				mIcon = value;

				if (mIcon != null)
					UpdateSize();
			}
		}

		public Padding Padding
		{
			get => mPadding;
			set
			{
				if (mPadding == value)
					return;

				mPadding = value;

				UpdateSize();
			}
		}

		/// Calculates the size of the button.
		private void UpdateSize()
		{
			float width = mPadding.Left + mIcon.mWidth + mPadding.Right;
			float height = mPadding.Top + mIcon.mHeight + mPadding.Bottom;

			Resize(mX, mY, width, height);
		}

        public override void Draw(Graphics g)
        {
            base.Draw(g);

			bool drawDown = ((mMouseDown && mMouseOver) || (mMouseFlags.HasFlag(MouseFlag.Kbd)));

			float radius = DarkTheme.SURFACE_RADIUS * DarkTheme.sScale;
			float borderWidth = DarkTheme.SURFACE_BORDER_WIDTH * DarkTheme.sScale;

			uint32 fillColor;
			if (mDisabled)
				fillColor = DarkTheme.COLOR_SURFACE_DISABLED;
			else if (drawDown)
				fillColor = DarkTheme.COLOR_SURFACE_PRESSED;
			else if (mMouseOver)
				fillColor = DarkTheme.COLOR_SURFACE_HOVER;
			else
				fillColor = DarkTheme.COLOR_SURFACE;

			using (g.PushColor(fillColor))
				g.FillRoundedRect(0, 0, mWidth, mHeight, radius);

			using (g.PushColor(DarkTheme.COLOR_SURFACE_BORDER))
				g.OutlineRoundedRect(0, 0, mWidth, mHeight, radius, borderWidth);

			if ((mHasFocus) && (!mDisabled))
			{
			    using (g.PushColor(DarkTheme.COLOR_SELECTED_OUTLINE))
			        g.OutlineRoundedRect(0, 0, mWidth, mHeight, radius, borderWidth);
			}

            g.Draw(mIcon, mPadding.Left, mPadding.Top);
        }
    }
}
