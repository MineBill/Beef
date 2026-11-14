using System;
using System.Collections;
using System.Text;
using Beefy.widgets;
using Beefy.gfx;
using Beefy.utils;

namespace Beefy.theme.dark
{
    public class DarkButton : ButtonWidget, IHotKeyHandler
    {
        public String mLabel ~ delete _;

        public override void Describe(Beefy.utils.StructuredData data)
        {
            base.Describe(data);
            if (mLabel != null)
                data.Add("label", mLabel);
        }
		public float mDrawDownPct;
		public FontAlign mLabelAlign = .Centered;
		public float mLabelXOfs;
		public float mLabelYOfs;

        [DesignEditable(DefaultEditString=true)]
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

		bool IHotKeyHandler.Handle(KeyCode keyCode)
		{
			if (mDisabled)
				return false;

			if (DarkTheme.CheckUnderlineKeyCode(mLabel, keyCode))
			{
				mDrawDownPct = 1.0f;
				MouseClicked(0, 0, 0, 0, 3);
				return true;
			}
			return false;
		}

        public override void DefaultDesignInit()
        {
            base.DefaultDesignInit();
            mIdStr = "Button";
            Label = "Button";
            mWidth = GS!(80);
            mHeight = GS!(20);
        }

        public override void Draw(Graphics g)
        {
            base.Draw(g);

			bool drawDown = ((mMouseDown && mMouseOver) || (mMouseFlags.HasFlag(MouseFlag.Kbd)));
			if (mDrawDownPct > 0)
				drawDown = true;

            uint32 fillColor;
            if (mDisabled)
                fillColor = DarkTheme.COLOR_SURFACE_DISABLED;
            else if (drawDown)
                fillColor = DarkTheme.COLOR_SURFACE_PRESSED;
            else if (mMouseOver)
                fillColor = DarkTheme.COLOR_SURFACE_HOVER;
            else
                fillColor = DarkTheme.COLOR_SURFACE;

            float radius = DarkTheme.SURFACE_RADIUS * DarkTheme.sScale;
            float borderWidth = DarkTheme.SURFACE_BORDER_WIDTH * DarkTheme.sScale;

            using (g.PushColor(fillColor))
                g.FillRoundedRect(0, 0, mWidth, mHeight, radius);

            using (g.PushColor(DarkTheme.COLOR_SURFACE_BORDER))
                g.OutlineRoundedRect(0, 0, mWidth, mHeight, radius, borderWidth);

            if ((mHasFocus) && (!mDisabled))
            {
                using (g.PushColor(DarkTheme.COLOR_SELECTED_OUTLINE))
                    g.OutlineRoundedRect(0, 0, mWidth, mHeight, radius, borderWidth);
            }

            g.SetFont(DarkTheme.sDarkTheme.mSmallFont);
            if (mLabel != null)
            {
                using (g.PushColor(mDisabled ? 0x80FFFFFF : Color.White))
                {
					using (g.PushColor(DarkTheme.COLOR_TEXT))
						DarkTheme.DrawUnderlined(g, mLabel, GS!(2) + mLabelXOfs, (mHeight - GS!(20)) / 2 + mLabelYOfs, mLabelAlign, mWidth - GS!(4) - mLabelXOfs, .Truncate);
                }
            }
        }

		public override void Update()
		{
			base.Update();
			if (mDrawDownPct > 0)
			{
				mDrawDownPct = Math.Max(mDrawDownPct - 0.25f, 0);
				MarkDirty();
			}
		}

        public override void Serialize(StructuredData data)
        {
            base.Serialize(data);
            data.Add("Label", mLabel);
        }

        public override bool Deserialize(StructuredData data)
        {
            base.Deserialize(data);
            data.GetString(mLabel, "Label");
            return true;
        }

		public override void GotFocus()
		{
			base.GotFocus();
		}
    }
}
