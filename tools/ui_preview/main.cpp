// Portable visual QA of the production ImGui view. Synthetic audio fixture;
// software rasterization, no audio device or Windows/DX11 emulation.
#include <overlay/OverlayRenderer.h>
#include <overlay/DashboardTheme.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>
using namespace EchoRadar;

static float Edge(ImVec2 a, ImVec2 b, ImVec2 p) {
    return (p.x-a.x)*(b.y-a.y) - (p.y-a.y)*(b.x-a.x);
}

int main(int argc, char** argv) {
    if (argc < 2) return 1; // output.ppm [page] [width] [height] [surround]
    const int width = argc > 3 ? std::clamp(std::atoi(argv[3]), 640, 3840) : 1440;
    const int height = argc > 4 ? std::clamp(std::atoi(argv[4]), 480, 2160) : 1000;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.IniFilename = nullptr;
#ifdef __APPLE__
    io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial.ttf", 17.0f);
#elif defined(_WIN32)
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
#else
    io.Fonts->AddFontDefault();
#endif
    unsigned char* atlas; int tw, th;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &tw, &th);
    ApplyDashboardTheme();
    OverlayRenderer view;
    view.SetPreviewPage(argc > 2 ? std::atoi(argv[2]) : 0);
    AppSnapshot snapshot;
    snapshot.capture.state = AudioCaptureState::Running;
    snapshot.capture.audioFresh = true;
    snapshot.capture.endpointName = "Headphones / Sound card output";
    snapshot.capture.sampleRate = 48000;
    snapshot.capture.signal = {true, -28.4f, -0.48f, true};
    snapshot.capture.levels.channelCount = 2;
    snapshot.capture.levels.peak[0] = 0.08f;
    snapshot.capture.levels.peak[1] = 0.03f;
    snapshot.layout.detected = *MakeAudioChannelLayout(2, WindowsSpeaker::Stereo);
    snapshot.layout.statusText = "Headphone mix ready. Front/rear azimuth is unknown.";
    snapshot.model.state = ModelUiState::Ready;
    snapshot.model.inferenceCount = 1842;
    snapshot.model.statusText = "Synthetic preview fixture / not a live recognition result";
    snapshot.model.probabilities = {0.12f, 0.78f};
    snapshot.settings.onboarding.completed = true;
    if (argc > 5) {
        snapshot.layout.detected = *MakeAudioChannelLayout(8, WindowsSpeaker::Surround71);
        snapshot.layout.directionalRadarAvailable = true;
        snapshot.radar.frame.status = RadarRuntimeStatus::Active;
        snapshot.radar.frame.sectorActivitiesDbfs.fill(-120.0f);
        snapshot.radar.frame.sectorActivitiesDbfs[5] = -16.0f;
        snapshot.radar.frame.sectorActivitiesDbfs[6] = -24.0f;
        snapshot.radar.frame.strongestAzimuthDegrees = 75.0f;
    }
    // Layouts stabilize after the first frame.
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame(); view.DrawDashboard(snapshot); ImGui::Render();
    }
    const ImDrawData* data = ImGui::GetDrawData();
    std::vector<unsigned char> pixels(width*height*3, 12);
    for (int n=0; n<data->CmdListsCount; ++n) {
        const auto* list = data->CmdLists[n];
        for (const auto& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            for (unsigned i=0; i+2<cmd.ElemCount; i+=3) {
                ImDrawVert v[3];
                for (int k=0;k<3;++k) v[k]=list->VtxBuffer[cmd.VtxOffset+list->IdxBuffer[cmd.IdxOffset+i+k]];
                const float area=Edge(v[0].pos,v[1].pos,v[2].pos);
                if (std::abs(area)<1e-8f) continue;
                int x0=std::max({0,(int)std::floor(std::min({v[0].pos.x,v[1].pos.x,v[2].pos.x})),(int)cmd.ClipRect.x});
                int y0=std::max({0,(int)std::floor(std::min({v[0].pos.y,v[1].pos.y,v[2].pos.y})),(int)cmd.ClipRect.y});
                int x1=std::min({width,(int)std::ceil(std::max({v[0].pos.x,v[1].pos.x,v[2].pos.x})),(int)cmd.ClipRect.z});
                int y1=std::min({height,(int)std::ceil(std::max({v[0].pos.y,v[1].pos.y,v[2].pos.y})),(int)cmd.ClipRect.w});
                for (int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
                    ImVec2 p(x+0.5f,y+0.5f);
                    float w[3]{Edge(v[1].pos,v[2].pos,p)/area,Edge(v[2].pos,v[0].pos,p)/area,Edge(v[0].pos,v[1].pos,p)/area};
                    if(w[0]<0 || w[1]<0 || w[2]<0) continue;
                    float u=0,t=0,col[4]{};
                    for(int k=0;k<3;++k) {
                        u+=w[k]*v[k].uv.x; t+=w[k]*v[k].uv.y;
                        for(int c=0;c<4;++c) col[c]+=w[k]*((v[k].col>>(8*c))&255);
                    }
                    int tex=4*(std::clamp((int)(t*th),0,th-1)*tw+std::clamp((int)(u*tw),0,tw-1));
                    float alpha=col[3]*atlas[tex+3]/65025.0f;
                    for(int c=0;c<3;++c) {
                        auto& dst=pixels[(y*width+x)*3+c];
                        dst=static_cast<unsigned char>(std::clamp(col[c]*atlas[tex+c]/255.0f*alpha+dst*(1-alpha),0.0f,255.0f));
                    }
                }
            }
        }
    }
    std::ofstream out(argv[1],std::ios::binary);
    out<<"P6\n"<<width<<" "<<height<<"\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
    return out ? 0 : 2;
}
