#include "openglworker.h"

#include "common/clamp.h"
#include "core.h"
#include "node/block/transition/transition.h"
#include "node/node.h"
#include "openglcolorprocessor.h"
#include "openglrenderfunctions.h"
#include "render/colormanager.h"
#include "render/pixelservice.h"

OpenGLWorker::OpenGLWorker(QOpenGLContext *share_ctx, OpenGLShaderCache *shader_cache, OpenGLTextureCache *texture_cache, VideoRenderFrameCache *frame_cache, QObject *parent) :
  VideoRenderWorker(frame_cache, parent),
  share_ctx_(share_ctx),
  ctx_(nullptr),
  functions_(nullptr),
  shader_cache_(shader_cache),
  texture_cache_(texture_cache)
{
  surface_.create();
}

OpenGLWorker::~OpenGLWorker()
{
  surface_.destroy();
}

bool OpenGLWorker::InitInternal()
{
  if (!VideoRenderWorker::InitInternal()) {
    return false;
  }

  // Create context object
  ctx_ = new QOpenGLContext();

  // Set share context
  ctx_->setShareContext(share_ctx_);

  // Create OpenGL context (automatically destroys any existing if there is one)
  if (!ctx_->create()) {
    qWarning() << "Failed to create OpenGL context in thread" << thread();
    return false;
  }

  ctx_->moveToThread(this->thread());

  // The rest of the initialization needs to occur in the other thread, so we signal for it to start
  QMetaObject::invokeMethod(this, "FinishInit", Qt::QueuedConnection);

  return true;
}

void OpenGLWorker::FrameToValue(StreamPtr stream, FramePtr frame, NodeValueTable *table)
{
  // Ensure stream is video or image type
  if (stream->type() != Stream::kVideo && stream->type() != Stream::kImage) {
    return;
  }

  ImageStreamPtr video_stream = std::static_pointer_cast<ImageStream>(stream);

  // Set up OCIO context
  OpenGLColorProcessorPtr color_processor = std::static_pointer_cast<OpenGLColorProcessor>(color_cache()->Get(video_stream->colorspace()));

  if (!color_processor) {
    // FIXME: We match with the colorspace string, but this won't change if the user sets a new config with a colorspace with the same string
    color_processor = OpenGLColorProcessor::CreateOpenGL(video_stream->footage()->project()->color_manager()->GetConfig(),
                                                         video_stream->colorspace(),
                                                         OCIO::ROLE_SCENE_LINEAR);
    color_cache()->Add(video_stream->colorspace(), color_processor);
  }

  ColorManager::OCIOMethod ocio_method = ColorManager::GetOCIOMethodForMode(video_params().mode());

  // OCIO's CPU conversion is more accurate, so for online we render on CPU but offline we render GPU
  if (ocio_method == ColorManager::kOCIOAccurate) {
    // If alpha is associated, disassociate for the color transform
    if (video_stream->premultiplied_alpha()) {
      ColorManager::DisassociateAlpha(frame);
    }

    // Convert frame to float for OCIO
    frame = PixelService::ConvertPixelFormat(frame, PixelFormat::PIX_FMT_RGBA32F);

    // Perform color transform
    color_processor->ConvertFrame(frame);

    // Associate alpha
    if (video_stream->premultiplied_alpha()) {
      ColorManager::ReassociateAlpha(frame);
    } else {
      ColorManager::AssociateAlpha(frame);
    }
  }

  VideoRenderingParams footage_params(frame->width(), frame->height(), stream->timebase(), frame->format(), video_params().mode());

  OpenGLTextureCache::ReferencePtr footage_tex_ref = texture_cache_->Get(ctx_, footage_params, frame->data());

  if (ocio_method == ColorManager::kOCIOFast) {
    if (!color_processor->IsEnabled()) {
      color_processor->Enable(ctx_, video_stream->premultiplied_alpha());
    }

    // Check frame aspect ratio
    if (frame->sample_aspect_ratio() != 1 && frame->sample_aspect_ratio() != 0) {
      int new_width = frame->width();
      int new_height = frame->height();

      // Scale the frame in a way that does not reduce the resolution
      if (frame->sample_aspect_ratio() > 1) {
        // Make wider
        new_width = qRound(static_cast<double>(new_width) * frame->sample_aspect_ratio().toDouble());
      } else {
        // Make taller
        new_height = qRound(static_cast<double>(new_height) / frame->sample_aspect_ratio().toDouble());
      }

      footage_params = VideoRenderingParams(new_width,
                                            new_height,
                                            footage_params.time_base(),
                                            footage_params.format(),
                                            footage_params.mode());
    }

    // Create destination texture
    OpenGLTextureCache::ReferencePtr associated_tex_ref = texture_cache_->Get(ctx_, footage_params);

    buffer_.Attach(associated_tex_ref->texture(), true);
    buffer_.Bind();
    footage_tex_ref->texture()->Bind();

    // Set viewport for texture size
    functions_->glViewport(0, 0, associated_tex_ref->texture()->width(), associated_tex_ref->texture()->height());

    // Blit old texture to new texture through OCIO shader
    color_processor->ProcessOpenGL();

    footage_tex_ref->texture()->Release();
    buffer_.Release();
    buffer_.Detach();

    footage_tex_ref = associated_tex_ref;
  }

  table->Push(NodeParam::kTexture, QVariant::fromValue(footage_tex_ref));
}

void OpenGLWorker::CloseInternal()
{
  buffer_.Destroy();
  functions_ = nullptr;
  delete ctx_;
}

void OpenGLWorker::ParametersChangedEvent()
{
  if (functions_ != nullptr && video_params().is_valid()) {
    functions_->glViewport(0, 0, video_params().effective_width(), video_params().effective_height());
  }
}

void OpenGLWorker::RunNodeAccelerated(const Node *node, const TimeRange &range, const NodeValueDatabase &input_params, NodeValueTable *output_params)
{
  OpenGLShaderPtr shader = shader_cache_->Get(node->id());

  if (!shader) {
    return;
  }

  // Create the output textures
  QList<OpenGLTextureCache::ReferencePtr> dst_refs;
  dst_refs.append(texture_cache_->Get(ctx_, video_params()));
  GLuint iterative_input = 0;

  // If this node requires multiple iterations, get a texture for it too
  if (node->AcceleratedCodeIterations() > 1 && node->AcceleratedCodeIterativeInput()) {
    dst_refs.append(texture_cache_->Get(ctx_, video_params()));
  }

  // Lock the shader so no other thread interferes as we set parameters and draw (and we don't interfere with any others)
  shader->Lock();
  shader->bind();

  unsigned int input_texture_count = 0;

  foreach (NodeParam* param, node->parameters()) {
    if (param->type() == NodeParam::kInput) {
      // See if the shader has takes this parameter as an input
      int variable_location = shader->uniformLocation(param->id());

      if (variable_location > -1) {
        // This variable is used in the shader, let's set it to our value

        NodeInput* input = static_cast<NodeInput*>(param);

        // Get value from database at this input
        const NodeValueTable& input_data = input_params[input];

        QVariant value = node->InputValueFromTable(input, input_data);

        switch (input->data_type()) {
        case NodeInput::kInt:
          shader->setUniformValue(variable_location, value.toInt());
          break;
        case NodeInput::kFloat:
          shader->setUniformValue(variable_location, value.toFloat());
          break;
        case NodeInput::kVec2:
          shader->setUniformValue(variable_location, value.value<QVector2D>());
          break;
        case NodeInput::kVec3:
          shader->setUniformValue(variable_location, value.value<QVector3D>());
          break;
        case NodeInput::kVec4:
          shader->setUniformValue(variable_location, value.value<QVector4D>());
          break;
        case NodeInput::kMatrix:
          shader->setUniformValue(variable_location, value.value<QMatrix4x4>());
          break;
        case NodeInput::kColor:
          shader->setUniformValue(variable_location, value.value<QColor>());
          break;
        case NodeInput::kBoolean:
          shader->setUniformValue(variable_location, value.toBool());
          break;
        case NodeInput::kFootage:
        case NodeInput::kTexture:
        case NodeInput::kBuffer:
        {
          OpenGLTextureCache::ReferencePtr texture = value.value<OpenGLTextureCache::ReferencePtr>();

          functions_->glActiveTexture(GL_TEXTURE0 + input_texture_count);

          GLuint tex_id = texture ? texture->texture()->texture() : 0;
          functions_->glBindTexture(GL_TEXTURE_2D, tex_id);

          // Set value to bound texture
          shader->setUniformValue(variable_location, input_texture_count);

          // Set enable flag if shader wants it
          int enable_param_location = shader->uniformLocation(QStringLiteral("%1_enabled").arg(input->id()));
          if (enable_param_location > -1) {
            shader->setUniformValue(enable_param_location,
                                    tex_id > 0);
          }

          if (tex_id > 0) {
            // Set texture resolution if shader wants it
            int res_param_location = shader->uniformLocation(QStringLiteral("%1_resolution").arg(input->id()));
            if (res_param_location > -1) {
              shader->setUniformValue(res_param_location,
                                      static_cast<GLfloat>(texture->texture()->width()),
                                      static_cast<GLfloat>(texture->texture()->height()));
            }
          }

          // If this texture binding is the iterative input, set it here
          if (input == node->AcceleratedCodeIterativeInput()) {
            iterative_input = input_texture_count;
          }

          OpenGLRenderFunctions::PrepareToDraw(functions_);

          input_texture_count++;
          break;
        }
        case NodeInput::kSamples:
        case NodeInput::kText:
        case NodeInput::kRational:
        case NodeInput::kFont:
        case NodeInput::kFile:
        case NodeInput::kDecimal:
        case NodeInput::kWholeNumber:
        case NodeInput::kNumber:
        case NodeInput::kString:
        case NodeInput::kVector:
        case NodeInput::kNone:
        case NodeInput::kAny:
          break;
        }
      }
    }
  }

  // Set up OpenGL parameters as necessary
  functions_->glViewport(0, 0, video_params().effective_width(), video_params().effective_height());

  // Provide some standard args
  shader->setUniformValue("ove_resolution",
                          static_cast<GLfloat>(video_params().width()),
                          static_cast<GLfloat>(video_params().height()));

  if (node->IsBlock()) {
    const Block* block_node = static_cast<const Block*>(node);

    if (block_node->type() == Block::kTransition) {
      const TransitionBlock* transition_node = static_cast<const TransitionBlock*>(node);

      // Provides total transition progress from 0.0 (start) - 1.0 (end)
      shader->setUniformValue("ove_tprog_all", static_cast<GLfloat>(transition_node->GetTotalProgress(range.in())));

      // Provides progress of out section from 1.0 (start) - 0.0 (end)
      shader->setUniformValue("ove_tprog_out", static_cast<GLfloat>(transition_node->GetOutProgress(range.in())));

      // Provides progress of in section from 0.0 (start) - 1.0 (end)
      shader->setUniformValue("ove_tprog_in", static_cast<GLfloat>(transition_node->GetInProgress(range.in())));
    }
  }

  // Some nodes use multiple iterations for optimization
  OpenGLTextureCache::ReferencePtr output_tex;

  for (int iteration=0;iteration<node->AcceleratedCodeIterations();iteration++) {
    // Set iteration number
    shader->bind();
    shader->setUniformValue("ove_iteration", iteration);
    shader->release();

    // If this is not the first iteration, set the parameter that will receive the last iteration's texture
    OpenGLTextureCache::ReferencePtr source_tex = dst_refs.at((iteration+1)%dst_refs.size());
    OpenGLTextureCache::ReferencePtr destination_tex = dst_refs.at(iteration%dst_refs.size());
    if (iteration > 0) {
      functions_->glActiveTexture(GL_TEXTURE0 + iterative_input);
      functions_->glBindTexture(GL_TEXTURE_2D, source_tex->texture()->texture());
    }

    buffer_.Attach(destination_tex->texture(), true);
    buffer_.Bind();

    // Blit this texture through this shader
    OpenGLRenderFunctions::Blit(shader);

    buffer_.Release();
    buffer_.Detach();

    // Update output reference to the last texture we wrote to
    output_tex = destination_tex;
  }

  // Make sure all OpenGL functions are complete by this point before unlocking the shader (or another thread may
  // change its parameters before our drawing in this thread is done)
  shader->Unlock();

  // Release any textures we bound before
  while (input_texture_count > 0) {
    input_texture_count--;

    // Release texture here
    functions_->glActiveTexture(GL_TEXTURE0 + input_texture_count);
    functions_->glBindTexture(GL_TEXTURE_2D, 0);
  }

  shader->release();

  output_params->Push(NodeParam::kTexture, QVariant::fromValue(output_tex));
}

void OpenGLWorker::TextureToBuffer(const QVariant &tex_in, QByteArray &buffer)
{
  OpenGLTextureCache::ReferencePtr texture = tex_in.value<OpenGLTextureCache::ReferencePtr>();

  PixelFormat::Info format_info = PixelService::GetPixelFormatInfo(video_params().format());

  texture->texture()->Lock();

  QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
  buffer_.Attach(texture->texture());
  buffer_.Bind();

  f->glReadPixels(0,
                  0,
                  texture->texture()->width(),
                  texture->texture()->height(),
                  format_info.pixel_format,
                  format_info.gl_pixel_type,
                  buffer.data());

  buffer_.Release();
  buffer_.Detach();

  texture->texture()->Unlock();
}

void OpenGLWorker::FinishInit()
{
  // Make context current on that surface
  if (!ctx_->makeCurrent(&surface_)) {
    qWarning() << "Failed to makeCurrent() on offscreen surface in thread" << thread();
    return;
  }

  // Store OpenGL functions instance
  functions_ = ctx_->functions();
  functions_->glBlendFunc(GL_ONE, GL_ZERO);

  ParametersChangedEvent();

  buffer_.Create(ctx_);
}
