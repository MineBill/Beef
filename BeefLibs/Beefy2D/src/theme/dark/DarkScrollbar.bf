using System;
using System.Collections;
using System.Text;
using Beefy.gfx;
using Beefy.widgets;

namespace Beefy.theme.dark
{
    public class DarkScrollbar : Scrollbar
    {
		public static uint32 BaseSize = 10;

        public class DarkThumb : Scrollbar.Thumb
        {               
            public override void Draw(Graphics g)
            {
                base.Draw(g);
                bool isOver = (mMouseOver || mMouseDown);
                using (g.PushColor(isOver ? DarkTheme.COLOR_SCROLLBAR_THUMB_HOVER : DarkTheme.COLOR_SCROLLBAR_THUMB))
                    g.FillRoundedRect(0, 0, mWidth, mHeight, Math.Min(mWidth, mHeight) * 0.5f);
            }
        }

        public class DarkArrow : Scrollbar.Arrow
        {            
            public this(bool isBack)
            {
                mBack = isBack;
            }

            public override void Draw(Graphics g)
            {				
                base.Draw(g);

                DarkScrollbar scrollbar = (DarkScrollbar)mScrollbar;

                if (mMouseOver)
                {
                    using (g.PushColor(mMouseDown ? DarkTheme.COLOR_SURFACE_PRESSED : DarkTheme.COLOR_SURFACE_HOVER))
                        g.FillRoundedRect(0, 0, mWidth, mHeight, DarkTheme.SURFACE_RADIUS * DarkTheme.sScale);
                }

                if (mBack)
                {
                    if (mScrollbar.mOrientation == Scrollbar.Orientation.Horz)
                    {
                        g.PushScale(-1.0f, 1);
                        g.Draw(scrollbar.GetImage(DarkTheme.ImageIdx.ScrollbarArrow), -DarkTheme.sUnitSize, 0);
                        g.PopMatrix();
                    }
                    else
                    {
                        g.PushScale(1, -1);
                        g.Draw(scrollbar.GetImage(DarkTheme.ImageIdx.ScrollbarArrow), 0, -DarkTheme.sUnitSize);
                        g.PopMatrix();
                    }
                }
                else
                {
                    g.Draw(scrollbar.GetImage(DarkTheme.ImageIdx.ScrollbarArrow));
                }
            }
        }

        public this()
        {
            mBaseSize = GS!(BaseSize);
            mDualBarSizeOffset = -2;

            mThumb = new DarkThumb();
            mThumb.mScrollbar = this;
            AddWidget(mThumb);
            
            mStartArrow = new DarkArrow(true);
            mStartArrow.mScrollbar = this;
            AddWidget(mStartArrow);
            
            mEndArrow = new DarkArrow(false);
            mEndArrow.mScrollbar = this;
            AddWidget(mEndArrow);
        }

		public override void RehupScale(float oldScale, float newScale)
		{
			mBaseSize = GS!(BaseSize);
			base.RehupScale(oldScale, newScale);
		}

        public Image GetImage(DarkTheme.ImageIdx image)
        {
            if (mOrientation == Orientation.Horz)
                return DarkTheme.sDarkTheme.GetImage(image);
            else
                return DarkTheme.sDarkTheme.GetImage((DarkTheme.ImageIdx)((int32)image + (int32)DarkTheme.ImageIdx.VertScrollbar - (int32)DarkTheme.ImageIdx.Scrollbar));
        }
        
        public override void Draw(Graphics g)
        {
            base.Draw(g);

            using (g.PushColor(DarkTheme.COLOR_SCROLLBAR))
                g.FillRect(0, 0, mWidth, mHeight);
        }

		public override void DrawAll(Graphics g)
		{
			if ((mWidth <= 0) || (mHeight <= 0))
				return;

			base.DrawAll(g);
		}

        public override double GetContentPosAt(float x, float y)
        {
            float btnMargin = GS!(18);
            float sizeLeft = (mOrientation == Orientation.Horz) ? (mWidth - btnMargin * 2) : (mHeight - btnMargin * 2);

            if (mOrientation == Orientation.Horz)
            {                
                float trackSize = sizeLeft - mThumb.mWidth;
                float trackPct = (x - btnMargin) / trackSize;
                double contentPos = (mContentSize - mPageSize) * trackPct;
                return contentPos + mContentStart;
            }
            else
            {
                float trackSize = sizeLeft - mThumb.mHeight;
                float trackPct = (y - btnMargin) / trackSize;
                double contentPos = (mContentSize - mPageSize) * trackPct;
                return contentPos + mContentStart;
            }
        }

        public override void ResizeContent()
        {
            mStartArrow.Resize(-mBaseSize / 2.0f, -mBaseSize / 2.0f, mBaseSize, mBaseSize);
            if (mOrientation == Orientation.Horz)
                mEndArrow.Resize(mWidth - mBaseSize, -mBaseSize / 2.0f, mBaseSize, mBaseSize);
            else
                mEndArrow.Resize(-mBaseSize / 2.0f, mHeight - mBaseSize, mBaseSize, mBaseSize);

            float btnMargin = GS!(18);

            float sizeLeft = (mOrientation == Orientation.Horz) ? (mWidth - btnMargin * 2) : (mHeight - btnMargin * 2);

            if ((mPageSize < mContentSize) && (mContentSize > 0))
            {
                double pagePct = mPageSize / mContentSize;
                mThumb.mVisible = true;
				
				if (sizeLeft >= GS!(12))
				{
					mThumb.mVisible = true;
					mStartArrow.mVisible = true;
					mEndArrow.mVisible = true;
				}
				else
				{						       
     				mThumb.mVisible = false;
					mStartArrow.mVisible = false;
					mEndArrow.mVisible = false;
	                /*sizeLeft = 0;
					sizeLeft = (mOrientation == Orientation.Horz) ? (mWidth) : (mHeight);
					btnMargin = 0;*/
				}

				double thumbSize = Math.Max(Math.Round(Math.Min(sizeLeft, Math.Max(GS!(12), sizeLeft * pagePct))), 0);
				double trackSize = Math.Max(sizeLeft - ((thumbSize) - (sizeLeft * pagePct)) + 1, 0);
				if (mOrientation == Orientation.Horz)
				    mThumb.Resize((float)(btnMargin + (float)Math.Round(mContentPos / mContentSize * trackSize)), 0, (float)thumbSize, mBaseSize);
				else
				    mThumb.Resize(0, (float)(btnMargin + (float)Math.Round(mContentPos / mContentSize * trackSize)), mBaseSize, (float)thumbSize);
            }
            else
            {
                mThumb.mVisible = false;
            }
        }

        public override void Resize(float x, float y, float width, float height)
        {
            base.Resize(x, y, width, height);

            ResizeContent();
        }
    }
}
