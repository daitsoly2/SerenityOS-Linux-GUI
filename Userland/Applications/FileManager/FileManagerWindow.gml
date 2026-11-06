@GUI::Widget {
    fill_with_background_color: true
    layout: @GUI::VerticalBoxLayout {
        spacing: 0
    }

    // Vista/Win7 Explorer chrome, top to bottom:
    //   1. command bar     (组织 / 共享 / 新建文件夹 / 预览窗格)
    //   2. address bar     (后退 / 前进 / 向上 / 面包屑 / 搜索)
    //   3. content host    (导航窗格 | 目录视图 | 预览窗格)
    //   4. status bar      (项目数 / 缩略图进度 / 视图按钮)
    // The custom widgets are created from C++ (ExplorerChrome.cpp) and mounted
    // into these named hosts, so GML stays a pure layout skeleton.

    @GUI::Widget {
        name: "command_bar_host"
        fixed_height: 32
    }

    @GUI::Widget {
        name: "address_bar_host"
        fixed_height: 34
    }

    @GUI::Widget {
        name: "content_host"
    }

    @GUI::Widget {
        name: "statusbar_host"
        fixed_height: 24
    }
}
