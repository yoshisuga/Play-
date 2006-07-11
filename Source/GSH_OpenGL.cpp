#include <stdio.h>
#include <assert.h>
#include <math.h>
#include "GSH_OpenGL.h"
#include "PS2VM.h"
#include "PtrMacro.h"
#include "RendererSettingsWnd.h"
#include "Config.h"

using namespace Framework;

PIXELFORMATDESCRIPTOR		CGSH_OpenGL::m_PFD =
{
	sizeof(PIXELFORMATDESCRIPTOR),
	1,
	PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
	PFD_TYPE_RGBA,
	32,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0,
	32,
	0,
	0,
	PFD_MAIN_PLANE,
	0,
	0, 0, 0
};

CGSH_OpenGL::CGSH_OpenGL(CWindow* pOutputWnd)
{
	m_pOutputWnd = pOutputWnd;
	m_pCvtBuffer = NULL;

	CConfig::GetInstance()->RegisterPreferenceBoolean(PREF_CGSH_OPENGL_LINEASQUADS, false);
	CConfig::GetInstance()->RegisterPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, false);

	m_nLinesAsQuads = CConfig::GetInstance()->GetPreferenceBoolean(PREF_CGSH_OPENGL_LINEASQUADS);
	m_nForceBilinearTextures = CConfig::GetInstance()->GetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES);

	InitializeRC();

	m_nVtxCount = 0;
	m_nWidth = -1;
	m_nHeight = -1;
	m_nTexCacheIndex = 0;

	m_nMaxZ = 32768.0;
}

CGSH_OpenGL::~CGSH_OpenGL()
{
	FREEPTR(m_pCvtBuffer);
	wglMakeCurrent(NULL, NULL);
	wglDeleteContext(m_hRC);
}

void CGSH_OpenGL::CreateGSHandler(CWindow* pOutputWnd)
{
	CPS2VM::CreateGSHandler(GSHandlerFactory, pOutputWnd);
}

void CGSH_OpenGL::LoadState(CStream* pStream)
{
	CGSHandler::LoadState(pStream);

	TexCache_InvalidateTextures(0, RAMSIZE);
}

void CGSH_OpenGL::InitializeRC()
{
	unsigned int pf;

	m_hDC = GetDC(m_pOutputWnd->m_hWnd);
	pf = ChoosePixelFormat(m_hDC, &m_PFD);
	SetPixelFormat(m_hDC, pf, &m_PFD);
	m_hRC = wglCreateContext(m_hDC);
	wglMakeCurrent(m_hDC, m_hRC);

	glewInit();
/*
	glColorTableEXT = (PFNGLCOLORTABLEEXTPROC)wglGetProcAddress("glColorTable");
	if(glColorTableEXT == NULL)
	{
		glColorTableEXT = (PFNGLCOLORTABLEEXTPROC)wglGetProcAddress("glColorTableEXT");
	}

	glBlendColorEXT	= (PFNGLBLENDCOLOREXTPROC)wglGetProcAddress("glBlendColor");
	if(glBlendColorEXT == NULL)
	{
		glBlendColorEXT	= (PFNGLBLENDCOLOREXTPROC)wglGetProcAddress("glBlendColorEXT");
	}

	glBlendEquationEXT = (PFNGLBLENDEQUATIONEXTPROC)wglGetProcAddress("glBlendEquation");
	if(glBlendEquationEXT == NULL)
	{
		glBlendEquationEXT = (PFNGLBLENDEQUATIONEXTPROC)wglGetProcAddress("glBlendEquationEXT");
	}

	glFogCoordfEXT = (PFNGLFOGCOORDFEXTPROC)wglGetProcAddress("glFogCoordfEXT");
*/
	//Initialize basic stuff
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(0.0f);
	glEnable(GL_TEXTURE_2D);

	//Initialize fog
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0.0f);
	glFogf(GL_FOG_END, 1.0f);
	glHint(GL_FOG_HINT, GL_NICEST);
	glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT);

//REMOVE
//	if(glColorTableEXT == NULL)
//	{
		m_pTexUploader_Psm8 = &CGSH_OpenGL::TexUploader_Psm8_Cvt;
//	}
//	else
//	{
//		m_pTexUploader_Psm8 = &CGSH_OpenGL::TexUploader_Psm8_Hw;
//	}

	VerifyRGBA5551Support();
	if(!m_nIsRGBA5551Supported)
	{
		m_pTexUploader_Psm16 = &CGSH_OpenGL::TexUploader_Psm16_Cvt;
	}
	else
	{
		m_pTexUploader_Psm16 = &CGSH_OpenGL::TexUploader_Psm16_Hw;
	}

	m_pProgram = NULL;
	m_pVertShader = m_pFragShader = NULL;

	//Create shaders/program
	if((glCreateProgram != NULL) && (glCreateShader != NULL))
	{
		m_pProgram		= new OpenGl::CProgram();
		m_pVertShader	= new OpenGl::CShader(GL_VERTEX_SHADER);
		m_pFragShader	= new OpenGl::CShader(GL_FRAGMENT_SHADER);

		LoadShaderSourceFromResource(m_pVertShader, _X("IDR_VERTSHADER"));
		LoadShaderSourceFromResource(m_pFragShader, _X("IDR_FRAGSHADER"));

		m_pVertShader->Compile();
		m_pFragShader->Compile();

		m_pProgram->AttachShader((*m_pVertShader));
		m_pProgram->AttachShader((*m_pFragShader));

		m_pProgram->Link();
	}

	m_pCvtBuffer = (uint8*)malloc(CVTBUFFERSIZE);

	m_pCLUT		= malloc(0x400);
	m_pCLUT32	= (uint32*)m_pCLUT;
	m_pCLUT16	= (uint16*)m_pCLUT;

	SetViewport(512, 384);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	SwapBuffers(m_hDC);
}

void CGSH_OpenGL::LoadShaderSourceFromResource(OpenGl::CShader* pShader, const xchar* sResourceName)
{
	const char* sSource;
	HGLOBAL nResourcePtr;
	HRSRC nResource;
	DWORD nSize;

	nResource		= FindResource(GetModuleHandle(NULL), sResourceName, _X("SHADER"));
	nResourcePtr	= LoadResource(GetModuleHandle(NULL), nResource);
	sSource			= const_cast<char*>(reinterpret_cast<char*>(LockResource(nResourcePtr)));
	nSize			= SizeofResource(GetModuleHandle(NULL), nResource);

	pShader->SetSource(sSource, nSize);
}

void CGSH_OpenGL::VerifyRGBA5551Support()
{
	unsigned int nTexture;

	m_nIsRGBA5551Supported = true;

	glGenTextures(1, &nTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 32, 32, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, m_pRAM);
	if(glGetError() == GL_INVALID_ENUM)
	{
		m_nIsRGBA5551Supported = false;
	}
	glDeleteTextures(1, &nTexture);
}

void CGSH_OpenGL::LinearZOrtho(double nLeft, double nRight, double nBottom, double nTop)
{
	double nMatrix[16];

	nMatrix[ 0] = 2.0 / (nRight - nLeft);
	nMatrix[ 1] = 0;
	nMatrix[ 2] = 0;
	nMatrix[ 3] = 0;

	nMatrix[ 4] = 0;
	nMatrix[ 5] = 2.0 / (nTop - nBottom);
	nMatrix[ 6] = 0;
	nMatrix[ 7] = 0;

	nMatrix[ 8] = 0;
	nMatrix[ 9] = 0;
	nMatrix[10] = 1;
	nMatrix[11] = 0;

	nMatrix[12] = - (nRight + nLeft) / (nRight - nLeft);
	nMatrix[13] = - (nTop + nBottom) / (nTop - nBottom);
	nMatrix[14] = 0;
	nMatrix[15] = 1;

	glMultMatrixd(nMatrix);
}

void CGSH_OpenGL::UpdateViewport()
{
	GSDISPLAY d;
	unsigned int nW, nH;

	if(m_nPMODE == 0) return;

	if(m_nPMODE & 0x01)
	{
		DECODE_DISPLAY(m_nDISPLAY1, d);
	}
	else
	{
		DECODE_DISPLAY(m_nDISPLAY2, d);
	}

	nW = (d.nW + 1) / (d.nMagX + 1);
	nH = (d.nH + 1);

	if(GetCrtIsInterlaced() && GetCrtIsFrameMode())
	{
		nH /= 2;
	}

	if(m_nWidth == nW && m_nHeight == nH)
	{
		return;
	}

	m_nWidth = nW;
	m_nHeight = nH;

//	SetViewport(m_nWidth, m_nHeight);
	SetViewport(GetCrtWidth(), GetCrtHeight());
//	SetViewport(320, 240);
	SetReadCircuitMatrix(m_nWidth, m_nHeight);

	/*
	float nRatio;
	nRatio = (float)GetCrtWidth() / (float)m_nWidth;

	if((nRatio - (int)nRatio) != 0)
	{
		glEnable(GL_LINE_SMOOTH);
	}
	else
	{
		glDisable(GL_LINE_SMOOTH);
	}

	glLineWidth(nRatio);
	*/
}

void CGSH_OpenGL::SetViewport(int nWidth, int nHeight)
{
	RECT rc;

	SetRect(&rc, 0, 0, nWidth, nHeight);
	AdjustWindowRect(&rc, GetWindowLong(m_pOutputWnd->m_hWnd, GWL_STYLE), FALSE);
	m_pOutputWnd->SetSize((rc.right - rc.left), (rc.bottom - rc.top));

	glViewport(0, 0, nWidth, nHeight);
}

void CGSH_OpenGL::SetReadCircuitMatrix(int nWidth, int nHeight)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	LinearZOrtho(0, nWidth, nHeight, 0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

uint32 CGSH_OpenGL::RGBA16ToRGBA32(uint16 nColor)
{
	return (nColor & 0x8000 ? 0xFF000000 : 0) | ((nColor & 0x7C00) << 9) | ((nColor & 0x03E0) << 6) | ((nColor & 0x001F) << 3);
}

uint8 CGSH_OpenGL::MulBy2Clamp(uint8 nValue)
{
	return (nValue > 0x7F) ? 0xFF : (nValue << 1);
}

double CGSH_OpenGL::GetZ(double nZ)
{
	if(nZ == 0)
	{
		return -1;
	}
	
	//if(nZ < 256)
	//{
	//	//The number is small, so scale to a smaller ratio (65536)
	//	return (nZ - 32768.0) / 32768.0;
	//}
	//else
	{
		nZ -= m_nMaxZ;
		if(nZ > m_nMaxZ) return 1.0;
		if(nZ < -m_nMaxZ) return -1.0;
		return nZ / m_nMaxZ;
	}
}

unsigned int CGSH_OpenGL::GetNextPowerOf2(unsigned int nNumber)
{
	return 1 << ((int)(log((double)(nNumber - 1)) / log(2.0)) + 1);
}

/////////////////////////////////////////////////////////////
// Context Unpacking
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::SetRenderingContext(unsigned int nContext)
{
	SetupBlendingFunction(m_nReg[GS_REG_ALPHA_1 + nContext]);
	SetupTestFunctions(m_nReg[GS_REG_TEST_1 + nContext]);
	SetupDepthBuffer(m_nReg[GS_REG_ZBUF_1 + nContext]);
	SetupTexture(m_nReg[GS_REG_TEX0_1 + nContext], m_nReg[GS_REG_TEX1_1 + nContext], m_nReg[GS_REG_CLAMP_1 + nContext]);
	
	DECODE_XYOFFSET(m_nReg[GS_REG_XYOFFSET_1 + nContext], m_nPrimOfsX, m_nPrimOfsY);
	
	if(GetCrtIsInterlaced() && GetCrtIsFrameMode())
	{
		if(m_nCSR & 0x2000)
		{
			m_nPrimOfsY += 0.5;
		}
	}
}

void CGSH_OpenGL::SetupBlendingFunction(uint64 nData)
{
	GSALPHA alpha;
	int nFunction;

	if(nData == 0) return;

	nFunction = GL_FUNC_ADD_EXT;

	DECODE_ALPHA(nData, alpha);

	if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 1) && (alpha.nFix == 0x80))
	{
		glBlendFunc(GL_ONE, GL_ONE);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 2) && (alpha.nD == 1) && (alpha.nFix == 0x80))
	{
		glBlendFunc(GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		//Source alpha value is implied in the formula
		//As = FIX / 0x80
		if(glBlendColorEXT != NULL)
		{
			glBlendColorEXT(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
			glBlendFunc(GL_CONSTANT_ALPHA_EXT, GL_ONE_MINUS_CONSTANT_ALPHA_EXT);
		}
	}
	else if((alpha.nA == 1) && (alpha.nB == 0) && (alpha.nC == 2) && (alpha.nD == 2))
	{
		nFunction = GL_FUNC_REVERSE_SUBTRACT_EXT;
		if(glBlendColorEXT != NULL)
		{
			glBlendColorEXT(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
			glBlendFunc(GL_CONSTANT_ALPHA_EXT, GL_CONSTANT_ALPHA_EXT);
		}
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	}
	else
	{
		printf("GSH_OpenGL: Unknown color blending formula.\r\n");
	}

	if(glBlendEquationEXT != NULL)
	{
		glBlendEquationEXT(nFunction);
	}
}

void CGSH_OpenGL::SetupTestFunctions(uint64 nData)
{
	GSTEST tst;
	unsigned int nFunc;
	float nValue;

	DECODE_TEST(nData, tst);

	if(tst.nAlphaEnabled)
	{
		switch(tst.nAlphaMethod)
		{
		case 0:
			nFunc = GL_NEVER;
			break;
		case 1:
			nFunc = GL_ALWAYS;
			break;
		case 2:
			nFunc = GL_LESS;
			break;
		case 6:
			nFunc = GL_GREATER;
			break;
		case 7:
			nFunc = GL_NOTEQUAL;
			break;
		default:
			assert(0);
			break;
		}

		nValue = (float)tst.nAlphaRef / 255.0f;
		glAlphaFunc(nFunc, nValue);

		//REMOVE
		glEnable(GL_ALPHA_TEST);

	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}

	if(tst.nDepthEnabled)
	{
		switch(tst.nDepthMethod)
		{
		case 0:
			nFunc = GL_NEVER;
			break;
		case 1:
			nFunc = GL_ALWAYS;
			break;
		case 2:
			nFunc = GL_GEQUAL;
			break;
		case 3:
			nFunc = GL_GREATER;
			break;
		}

		glDepthFunc(nFunc);

		glEnable(GL_DEPTH_TEST);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
	}
}

void CGSH_OpenGL::SetupDepthBuffer(uint64 nData)
{
	ZBUF* zbuf;

	zbuf = (ZBUF*)&nData;

	switch(GetPsmPixelSize(zbuf->nPsm))
	{
	case 16:
		m_nMaxZ = 32768.0;
		break;
	case 32:
		m_nMaxZ = 2147483647.0;
		break;
	}
}

void CGSH_OpenGL::SetupTexture(uint64 nTex0, uint64 nTex1, uint64 nClamp)
{
	GSTEX0 tex0;
	GSTEX1 tex1;
	CLAMP clamp;

	if(nTex0 == 0)
	{
		m_nTexHandle = 0;
		return;
	}

	DECODE_TEX0(nTex0, tex0);
	DECODE_TEX1(nTex1, tex1);
	clamp = *(CLAMP*)&nClamp;

	m_nTexWidth		= tex0.GetWidth();
	m_nTexHeight	= tex0.GetHeight();
	m_nTexHandle	= LoadTexture(&tex0, &tex1, &clamp);
}

void CGSH_OpenGL::SetupFogColor()
{
	float nColor[4];
	FOGCOL* pColor;

	pColor = GetFogCol();
	nColor[0] = (float)pColor->nFCR / 255.0f;
	nColor[1] = (float)pColor->nFCG / 255.0f;
	nColor[2] = (float)pColor->nFCB / 255.0f;
	nColor[3] = 0.0f;

	glFogfv(GL_FOG_COLOR, nColor);
}

/////////////////////////////////////////////////////////////
// Individual Primitives Implementations
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::Prim_Point()
{
	double nX, nY, nZ;
	GSRGBAQ rgbaq;
	
	DECODE_XYZ2(m_VtxBuffer[0].nPosition, nX, nY, nZ);

	nX -= m_nPrimOfsX;
	nY -= m_nPrimOfsY;

	if(!m_PrimitiveMode.nUseUV && !m_PrimitiveMode.nTexture)
	{
		DECODE_RGBAQ(m_VtxBuffer[0].nRGBAQ, rgbaq);

		glBegin(GL_QUADS);

			glColor4ub(rgbaq.nR, rgbaq.nG, rgbaq.nB, rgbaq.nA);

			glVertex2d(nX + 0, nY + 0);
			glVertex2d(nX + 1, nY + 0);
			glVertex2d(nX + 1, nY + 1);
			glVertex2d(nX + 0, nY + 1);

		glEnd();

	}
	else
	{
		//Yay for textured points!
		assert(0);
	}
}

void CGSH_OpenGL::Prim_Line()
{
	double nX1, nX2;
	double nY1, nY2;
	double nZ;
	GSRGBAQ rgbaq[2];
	
	DECODE_XYZ2(m_VtxBuffer[1].nPosition, nX1, nY1, nZ);
	DECODE_XYZ2(m_VtxBuffer[0].nPosition, nX2, nY2, nZ);

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

	if(m_PrimitiveMode.nAntiAliasing)
	{
		glEnable(GL_BLEND);
	}

	if(!m_PrimitiveMode.nUseUV && !m_PrimitiveMode.nTexture)
	{

		DECODE_RGBAQ(m_VtxBuffer[1].nRGBAQ, rgbaq[0]);
		DECODE_RGBAQ(m_VtxBuffer[0].nRGBAQ, rgbaq[1]);

		if(m_nLinesAsQuads)
		{
			glBegin(GL_QUADS);

				glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, rgbaq[0].nA);
		
				glVertex2d(nX1 + 0, nY1 + 0);
				glVertex2d(nX1 + 1, nY1 + 1);

				glColor4ub(rgbaq[1].nR, rgbaq[1].nG, rgbaq[1].nB, rgbaq[1].nA);

				glVertex2d(nX2 + 1, nY2 + 1);
				glVertex2d(nX2 + 0, nY2 + 0);

			glEnd();
		}
		else
		{
			glBegin(GL_LINES);

				glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, rgbaq[0].nA);
				glVertex2d(nX1, nY1);

				glColor4ub(rgbaq[1].nR, rgbaq[1].nG, rgbaq[1].nB, rgbaq[1].nA);
				glVertex2d(nX2, nY2);

			glEnd();
		}
	}
	else
	{

		//Yay for textured lines!
		assert(0);
	}

	if(m_PrimitiveMode.nAntiAliasing)
	{
		glDisable(GL_BLEND);
	}
}

void CGSH_OpenGL::Prim_Triangle()
{
	double nX1, nX2, nX3;
	double nY1, nY2, nY3;
	double nZ1, nZ2, nZ3;

	double nU1, nU2, nU3;
	double nV1, nV2, nV3;

	double nS1, nS2, nS3;
	double nT1, nT2, nT3;

	float nF1, nF2, nF3;

	GSRGBAQ rgbaq[3];

	DECODE_XYZ2(m_VtxBuffer[2].nPosition, nX1, nY1, nZ1);
	DECODE_XYZ2(m_VtxBuffer[1].nPosition, nX2, nY2, nZ2);
	DECODE_XYZ2(m_VtxBuffer[0].nPosition, nX3, nY3, nZ3);

	DECODE_RGBAQ(m_VtxBuffer[2].nRGBAQ, rgbaq[0]);
	DECODE_RGBAQ(m_VtxBuffer[1].nRGBAQ, rgbaq[1]);
	DECODE_RGBAQ(m_VtxBuffer[0].nRGBAQ, rgbaq[2]);

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;
	nX3 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;
	nY3 -= m_nPrimOfsY;

	nZ1 = GetZ(nZ1);
	nZ2 = GetZ(nZ2);
	nZ3 = GetZ(nZ3);

	if(m_PrimitiveMode.nShading)
	{
		glShadeModel(GL_SMOOTH);
	}
	else
	{
		glShadeModel(GL_FLAT);
	}

	if(m_PrimitiveMode.nAlpha)
	{
		glEnable(GL_BLEND);
	}

	if(m_PrimitiveMode.nFog)
	{
		glEnable(GL_FOG);

		nF1 = (float)(0xFF - m_VtxBuffer[2].nFog) / 255.0f;
		nF2 = (float)(0xFF - m_VtxBuffer[1].nFog) / 255.0f;
		nF3 = (float)(0xFF - m_VtxBuffer[0].nFog) / 255.0f;
	}
	else
	{
		nF1 = nF2 = nF3 = 0.0;
	}

	if(m_PrimitiveMode.nTexture)
	{
		//Textured triangle

		if(m_PrimitiveMode.nUseUV)
		{
			glBindTexture(GL_TEXTURE_2D, m_nTexHandle);

			DECODE_UV(m_VtxBuffer[2].nUV, nU1, nV1);
			DECODE_UV(m_VtxBuffer[1].nUV, nU2, nV2);
			DECODE_UV(m_VtxBuffer[0].nUV, nU3, nV3);

			nU1 /= (double)m_nTexWidth;
			nU2 /= (double)m_nTexWidth;
			nU3 /= (double)m_nTexWidth;

			nV1 /= (double)m_nTexHeight;
			nV2 /= (double)m_nTexHeight;
			nV3 /= (double)m_nTexHeight;

			glBegin(GL_TRIANGLES);
			{
				glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));
				glTexCoord2d(nU1, nV1);
				glVertex3d(nX1, nY1, nZ1);

				glColor4ub(MulBy2Clamp(rgbaq[1].nR), MulBy2Clamp(rgbaq[1].nG), MulBy2Clamp(rgbaq[1].nB), MulBy2Clamp(rgbaq[1].nA));
				glTexCoord2d(nU2, nV2);
				glVertex3d(nX2, nY2, nZ2);

				glColor4ub(MulBy2Clamp(rgbaq[2].nR), MulBy2Clamp(rgbaq[2].nG), MulBy2Clamp(rgbaq[2].nB), MulBy2Clamp(rgbaq[2].nA));
				glTexCoord2d(nU3, nV3);
				glVertex3d(nX3, nY3, nZ3);
			}
			glEnd();

			glBindTexture(GL_TEXTURE_2D, NULL);

		}
		else
		{
			glBindTexture(GL_TEXTURE_2D, m_nTexHandle);

			DECODE_ST(m_VtxBuffer[2].nST, nS1, nT1);
			DECODE_ST(m_VtxBuffer[1].nST, nS2, nT2);
			DECODE_ST(m_VtxBuffer[0].nST, nS3, nT3);

			nS1 /= rgbaq[0].nQ;
			nS2 /= rgbaq[1].nQ;
			nS3 /= rgbaq[2].nQ;

			nT1 /= rgbaq[0].nQ;
			nT2 /= rgbaq[1].nQ;
			nT3 /= rgbaq[2].nQ;

			glBegin(GL_TRIANGLES);
			{
				glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));
				glTexCoord2d(nS1, nT1);
				if(glFogCoordfEXT) glFogCoordfEXT(nF1);
				glVertex3d(nX1, nY1, nZ1);

				glColor4ub(MulBy2Clamp(rgbaq[1].nR), MulBy2Clamp(rgbaq[1].nG), MulBy2Clamp(rgbaq[1].nB), MulBy2Clamp(rgbaq[1].nA));
				glTexCoord2d(nS2, nT2);
				if(glFogCoordfEXT) glFogCoordfEXT(nF2);
				glVertex3d(nX2, nY2, nZ2);

				glColor4ub(MulBy2Clamp(rgbaq[2].nR), MulBy2Clamp(rgbaq[2].nG), MulBy2Clamp(rgbaq[2].nB), MulBy2Clamp(rgbaq[2].nA));
				glTexCoord2d(nS3, nT3);
				if(glFogCoordfEXT) glFogCoordfEXT(nF3);
				glVertex3d(nX3, nY3, nZ3);
			}
			glEnd();

			glBindTexture(GL_TEXTURE_2D, NULL);			
		}
	}
	else
	{
		//Non Textured Triangle
		glBegin(GL_TRIANGLES);
			
			glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, (uint8)min(0xFF, (int)rgbaq[0].nA * 2));
			glVertex3d(nX1, nY1, nZ1);

			glColor4ub(rgbaq[1].nR, rgbaq[1].nG, rgbaq[1].nB, (uint8)min(0xFF, (int)rgbaq[1].nA * 2));
			glVertex3d(nX2, nY2, nZ2);

			glColor4ub(rgbaq[2].nR, rgbaq[2].nG, rgbaq[2].nB, (uint8)min(0xFF, (int)rgbaq[2].nA * 2));
			glVertex3d(nX3, nY3, nZ3);

		glEnd();
	}

	if(m_PrimitiveMode.nFog)
	{
		glDisable(GL_FOG);
	}

	if(m_PrimitiveMode.nAlpha)
	{
		glDisable(GL_BLEND);
	}
}

void CGSH_OpenGL::Prim_Sprite()
{
	double nX1, nX2;
	double nY1, nY2;
	double nZ1, nZ2;

	double nU1, nU2;
	double nV1, nV2;

	GSRGBAQ rgbaq[2];

	DECODE_XYZ2(m_VtxBuffer[1].nPosition, nX1, nY1, nZ1);
	DECODE_XYZ2(m_VtxBuffer[0].nPosition, nX2, nY2, nZ2);

	DECODE_RGBAQ(m_VtxBuffer[1].nRGBAQ, rgbaq[0]);
	DECODE_RGBAQ(m_VtxBuffer[0].nRGBAQ, rgbaq[1]);

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

//	assert(nZ1 == nZ2);

	nZ1 = GetZ(nZ1);
	nZ2 = GetZ(nZ2);

	if(m_PrimitiveMode.nAlpha)
	{
		glEnable(GL_BLEND);
	}

	if(m_PrimitiveMode.nUseUV && m_PrimitiveMode.nTexture)
	{
		glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));

		glBindTexture(GL_TEXTURE_2D, m_nTexHandle);

		DECODE_UV(m_VtxBuffer[1].nUV, nU1, nV1);
		DECODE_UV(m_VtxBuffer[0].nUV, nU2, nV2);

		nU1 /= (double)m_nTexWidth;
		nU2 /= (double)m_nTexWidth;

		nV1 /= (double)m_nTexHeight;
		nV2 /= (double)m_nTexHeight;

		glBegin(GL_QUADS);
		{
			//REMOVE
			//glColor4d(1.0, 1.0, 1.0, 1.0);

			glTexCoord2d(nU1, nV1);
			glVertex3d(nX1, nY1, nZ1);

			glTexCoord2d(nU2, nV1);
			glVertex3d(nX2, nY1, nZ2);

			//REMOVE
			//glColor4d(0.5, 0.5, 0.5, 1.0);

			glTexCoord2d(nU2, nV2);
			glVertex3d(nX2, nY2, nZ1);

			glTexCoord2d(nU1, nV2);
			glVertex3d(nX1, nY2, nZ2);
		}
		glEnd();

		glBindTexture(GL_TEXTURE_2D, 0);
	}
	else if(!m_PrimitiveMode.nTexture)
	{
		//REMOVE
		//Humm? Would it be possible to have a gradient using those registers?
		glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));
		//glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, rgbaq[0].nA);

		glBegin(GL_QUADS);

			glVertex3d(nX1, nY1, nZ1);
			glVertex3d(nX2, nY1, nZ2);
			glVertex3d(nX2, nY2, nZ1);
			glVertex3d(nX1, nY2, nZ2);

		glEnd();
	}
	else
	{
		assert(0);
	}

	if(m_PrimitiveMode.nAlpha)
	{
		glDisable(GL_BLEND);
	}
}

/////////////////////////////////////////////////////////////
// Other Functions
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::WriteRegister(uint8 nRegister, uint64 nData)
{
	CGSHandler::WriteRegister(nRegister, nData);
	
	switch(nRegister)
	{
	case GS_REG_PRIM:
		m_nPrimitiveType = (unsigned int)(nData & 0x07);
		switch(m_nPrimitiveType)
		{
		case 0:
			//Point
			m_nVtxCount = 1;
			break;
		case 1:
			//Line
			m_nVtxCount = 2;
			break;
		case 2:
			//Line strip
			m_nVtxCount = 2;
			break;
		case 3:
			//Triangle
			m_nVtxCount = 3;
			break;
		case 4:
			//Triangle Strip
			m_nVtxCount = 3;
			break;
		case 5:
			//Triangle Fan
			m_nVtxCount = 3;
			break;
		case 6:
			//Sprite (rectangle)
			m_nVtxCount = 2;
			break;
		default:
			printf("GS: Unhandled primitive type (%i) encountered.\r\n", m_nPrimitiveType);
			break;
		}
		break;

	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(nRegister, nData);
		break;

	case GS_REG_TEX2_1:
	case GS_REG_TEX2_2:
		{
			unsigned int nContext;
			const uint64 nMask = 0xFFFFFFE003F00000;
			GSTEX0 Tex0;

			nContext = nRegister - GS_REG_TEX2_1;

			Tex0 = *(GSTEX0*)&m_nReg[GS_REG_TEX0_1 + nContext];
			if(Tex0.nCLD == 1 && Tex0.nCPSM == 0)
			{
				ReadCLUT8(&Tex0);
			}

			m_nReg[GS_REG_TEX0_1 + nContext] &= ~nMask;
			m_nReg[GS_REG_TEX0_1 + nContext] |= nData & nMask;
		}
		break;

	case GS_REG_FOGCOL:
		SetupFogColor();
		break;
	}
}

void CGSH_OpenGL::VertexKick(uint8 nRegister, uint64 nValue)
{
	bool nFog;
	bool nDrawingKick;

	if(m_nVtxCount == 0) return;

	nDrawingKick = (nRegister == GS_REG_XYZ2) || (nRegister == GS_REG_XYZF2);
	nFog = (nRegister == GS_REG_XYZF2) || (nRegister == GS_REG_XYZF3);

	if(nFog)
	{
		m_VtxBuffer[m_nVtxCount - 1].nPosition	= nValue & 0x00FFFFFFFFFFFFFF;
		m_VtxBuffer[m_nVtxCount - 1].nRGBAQ		= m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].nUV		= m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].nST		= m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].nFog		= (uint8)(nValue >> 56);
	}
	else
	{
		m_VtxBuffer[m_nVtxCount - 1].nPosition	= nValue;
		m_VtxBuffer[m_nVtxCount - 1].nRGBAQ		= m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].nUV		= m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].nST		= m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].nFog		= (uint8)(m_nReg[GS_REG_FOG] >> 56);
	}

	m_nVtxCount--;

	if(m_nVtxCount == 0)
	{
		if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
		{
			DECODE_PRMODE(m_nReg[GS_REG_PRIM], m_PrimitiveMode);
		}
		else
		{
			DECODE_PRMODE(m_nReg[GS_REG_PRMODE], m_PrimitiveMode);
		}

		SetRenderingContext(m_PrimitiveMode.nContext);

		switch(m_nPrimitiveType)
		{
		case 0:
			if(nDrawingKick) Prim_Point();
			break;
		case 1:
			if(nDrawingKick) Prim_Line();
			break;
		case 2:
			if(nDrawingKick) Prim_Line();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case 3:
			if(nDrawingKick) Prim_Triangle();
			m_nVtxCount = 3;
			break;
		case 4:
			if(nDrawingKick) Prim_Triangle();
			memcpy(&m_VtxBuffer[2], &m_VtxBuffer[1], sizeof(VERTEX));
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case 5:
			if(nDrawingKick) Prim_Triangle();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case 6:
			if(nDrawingKick) Prim_Sprite();
			m_nVtxCount = 2;
			break;
		}
	}
}

void CGSH_OpenGL::ProcessImageTransfer(uint32 nAddress, uint32 nLenght)
{
	BITBLTBUF* pBuf;
	FRAME* pFrame;
	uint32 nFrameEnd;

	pFrame = GetFrame(0);
	pBuf = GetBitBltBuf();

	TexCache_InvalidateTextures(nAddress, nLenght);

	nFrameEnd = (pFrame->GetBasePtr() + (pFrame->GetWidth() * GetPsmPixelSize(pFrame->nPsm) / 8) * m_nHeight);
	if(nAddress < nFrameEnd)
	{
		if((pFrame->nPsm == PSMCT24) && (pBuf->nDstPsm == PSMT4HH || pBuf->nDstPsm == PSMT4HL || pBuf->nDstPsm == PSMT8H)) return;

		DisplayTransferedImage(nAddress);
	}
}

void CGSH_OpenGL::DisplayTransferedImage(uint32 nAddress)
{
	unsigned int nW, nH;
	unsigned int nW2, nH2;
	unsigned int nDX, nDY;
	unsigned int nTexture;
	uint32* pImg;
	double nS, nT;
	TRXREG* pReg;
	TRXPOS* pPos;

	pReg = GetTrxReg();
	pPos = GetTrxPos();

	nW = pReg->nRRW;
	nH = pReg->nRRH;

	nDX = pPos->nDSAX;
	nDY = pPos->nDSAY;

	pImg = (uint32*)(m_pRAM + nAddress);

	glGenTextures(1, &nTexture);

	glBindTexture(GL_TEXTURE_2D, nTexture);

	nW2 = GetNextPowerOf2(nW);
	nH2 = GetNextPowerOf2(nH);

	//Upload the texture
	glPixelStorei(GL_UNPACK_ROW_LENGTH, nW);

	glTexImage2D(GL_TEXTURE_2D, 0, 4, nW2, nH2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pImg);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	glColor4d(1.0, 1.0, 1.0, 1.0);

	nS = (double)nW / (double)nW2;
	nT = (double)nH / (double)nH2;

	glBegin(GL_QUADS);
		
		glTexCoord2d(0.0,		0.0);
		glVertex2d(nDX,			nDY);

		glTexCoord2d(0.0,		nT);
		glVertex2d(nDX,			nDY + nH);

		glTexCoord2d(nS,		nT);
		glVertex2d(nDX + nW,	nDY + nH);

		glTexCoord2d(nS,		0.0);
		glVertex2d(nDX + nW,	nDY);

	glEnd();

	glBindTexture(GL_TEXTURE_2D, NULL);

	glDeleteTextures(1, &nTexture);
}

void CGSH_OpenGL::Flip()
{
	SwapBuffers(m_hDC);
}

CSettingsDialogProvider* CGSH_OpenGL::GetSettingsDialogProvider()
{
	return this;
}

CModalWindow* CGSH_OpenGL::CreateSettingsDialog(HWND hParent)
{
	return new CRendererSettingsWnd(hParent, this);
}

void CGSH_OpenGL::OnSettingsDialogDestroyed()
{
	m_nLinesAsQuads = CConfig::GetInstance()->GetPreferenceBoolean(PREF_CGSH_OPENGL_LINEASQUADS);
	m_nForceBilinearTextures = CConfig::GetInstance()->GetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES);
	TexCache_Flush();
}

bool CGSH_OpenGL::IsColorTableExtSupported()
{
	return glColorTableEXT != NULL;
}

bool CGSH_OpenGL::IsBlendColorExtSupported()
{
	return glBlendColorEXT != NULL;
}

bool CGSH_OpenGL::IsBlendEquationExtSupported()
{
	return glBlendEquationEXT != NULL;
}

bool CGSH_OpenGL::IsRGBA5551ExtSupported()
{
	return m_nIsRGBA5551Supported;
}

bool CGSH_OpenGL::IsFogCoordfExtSupported()
{
	return glFogCoordfEXT != NULL;
}

CGSHandler* CGSH_OpenGL::GSHandlerFactory(void* pParam)
{
	return new CGSH_OpenGL((CWindow*)pParam);
}
