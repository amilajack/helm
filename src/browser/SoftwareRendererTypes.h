// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
using GLuint = unsigned;
struct OpenGLShaderProgram { struct Uniform {}; struct Attribute {}; };
struct OpenGLTexture {};
struct OpenGLContext {
  enum OpenGLVersion { openGL3_2 };
  void setContinuousRepainting(bool) {}
  template<class T> void setRenderer(T*) {}
  void setRenderer(std::nullptr_t) {}
  template<class T> void attachTo(T&) {}
  void setOpenGLVersionRequired(OpenGLVersion) {}
  void detach() {}
};
struct OpenGLRenderer {
  virtual ~OpenGLRenderer() = default;
  virtual void newOpenGLContextCreated() = 0;
  virtual void renderOpenGL() = 0;
  virtual void openGLContextClosing() = 0;
};
