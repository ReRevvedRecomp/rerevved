#pragma once

#include <rex/rex_app.h>

// Defined by the generated module init (generated/default/rerevved_init.cpp).
extern const rex::PPCImageInfo PPCImageConfig;

class ReRevvedApp : public rex::ReXApp
{
public:
    using rex::ReXApp::ReXApp;

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx)
    {
        return std::unique_ptr<ReRevvedApp>(new ReRevvedApp(ctx, "rerevved", PPCImageConfig));
    }

protected:
    void OnConfigurePaths(rex::PathConfig& paths) override;

    bool SetupEnvironment() override;

    bool SetupPresentation() override;

    std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig&               defaults,
                                                   std::function<void(rex::PathConfig)> resume) override;

    void OnPostSetup() override;

    void OnShutdown() override;

    bool OnWindowCloseRequested() override;
};
