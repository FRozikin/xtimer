//<script>
// Time Input Formatter
function formatTimeInputValue(input) {
  // Pastikan format hh:mm:ss
  let value = input.value;
  if (value && value.length === 5) {
    // Jika hanya hh:mm
    value = value + ":00"; // Tambahkan detik default
  }
  // Tambahkan validasi sederhana untuk memastikan format hh:mm:ss
  if (!/^\d{2}:\d{2}:\d{2}$/.test(value)) {
    // Fallback atau peringatan jika format tidak sesuai
    console.warn("Invalid time format, defaulting to 00:00:00");
    return "00:00:00";
  }
  return value;
}

// Inisialisasi tab
function initTabs() {
  const tabsContainer = document.getElementById("channelTabs");
  const contentContainer = document.getElementById("channelContents");

  for (let i = 0; i < 8; i++) {
    const tabBtn = document.createElement("button");
    tabBtn.className = "tab-button";
    tabBtn.textContent = `Ch.${i + 1}`;
    tabBtn.onclick = () => showTab(i);
    tabsContainer.appendChild(tabBtn);

    const tabContent = document.createElement("div");
    tabContent.className = "tab-content";
    tabContent.id = `tab-${i}`;
    tabContent.style.display = i === 0 ? "block" : "none";
    tabContent.innerHTML = `
      <table>
        <thead>
          <tr>
            <th>ON</th>
            <th>OFF</th>
            <th>Enable</th>
            <th>Active Days</th> <th>Action <button class="add-btn" data-channel="${i}" data-tooltip="Add Timer">➕</button></th>
          </tr>
        </thead>
        <tbody id="config-${i}"></tbody>
      </table>
      <div class="channel-manual-control" style="padding: 10px; text-align: center; background: #e9e9e9; margin-top: 10px; border-radius: 5px;">
        Status: <span id="status-ch${i}" class="status-indicator">OFF</span>
        <button class="manual-control-btn override-on-btn" data-channel="${i}" data-tooltip="Activate Manual Control">Override ON</button>
        <button class="manual-control-btn on-btn" data-channel="${i}" data-state="true" disabled data-tooltip="Turn ON Manually (requires Override ON)">ON</button>
        <button class="manual-control-btn off-btn" data-channel="${i}" data-state="false" disabled data-tooltip="Turn OFF Manually (requires Override ON)">OFF</button>
        <button class="manual-control-btn override-off-btn" data-channel="${i}" data-tooltip="Deactivate Manual Control">Override OFF</button>
      </div>
    `;
    contentContainer.appendChild(tabContent);
  }

  // Set tab pertama sebagai active
  if (tabsContainer.firstChild) {
    tabsContainer.firstChild.classList.add("active");
  }
}

function showTab(index) {
  // Update tab buttons
  document.querySelectorAll(".tab-button").forEach((btn) => {
    btn.classList.remove("active");
  });
  document.querySelectorAll(".tab-button")[index].classList.add("active");

  // Update tab content
  document.querySelectorAll(".tab-content").forEach((tab) => {
    tab.style.display = "none";
  });
  document.getElementById(`tab-${index}`).style.display = "block";
}

// NEW: Update addTimerRow to include day checkboxes
function addTimerRow(
  channel,
  config = {
    onTime: "00:00:00",
    offTime: "00:00:00",
    enable: true,
    activeDays: Array(7).fill(true),
  }
) {
  // NEW: Default activeDays ke semua true
  const tbody = document.getElementById(`config-${channel}`);
  const row = document.createElement("tr");
  const dayLabels = ["Sen", "Sel", "Rab", "Kam", "Jum", "Sab", "Min"]; // Label hari untuk UI

  // Buat HTML untuk checkbox hari
  let dayCheckboxesHtml = '<div class="day-checkboxes">';
  for (let k = 0; k < 7; k++) {
    dayCheckboxesHtml += `
        <label>
          <input type="checkbox" ${
            config.activeDays[k] ? "checked" : ""
          } data-day-index="${k}">
          ${dayLabels[k]}
        </label>
      `;
  }
  dayCheckboxesHtml += "</div>";

  row.innerHTML = `
            <td><input type="time" step="1" class="time-input" value="${
              config.onTime
            }" onchange="this.value = formatTimeInputValue(this)" onblur="validateTimeInput(this)"></td>
            <td><input type="time" step="1" class="time-input" value="${
              config.offTime
            }" onchange="this.value = formatTimeInputValue(this)" onblur="validateTimeInput(this)"></td>
            <td><input type="checkbox" ${config.enable ? "checked" : ""}></td>
            <td>${dayCheckboxesHtml}</td>
            <td><button class="delete-btn" data-tooltip="Delete">❌</button></td>
  `;
  tbody.appendChild(row);
  // NEW: Tambahkan ini SETELAH `tbody.appendChild(row);`
  // Ini memastikan input sudah ada di DOM sebelum divalidasi
  const newTimeInputs = row.querySelectorAll(".time-input");
  newTimeInputs.forEach((input) => {
    validateTimeInput(input); // Lakukan validasi awal saat baris dibuat/dimuat
  });
}

function loadConfig() {
  // Mengembalikan Promise agar kita bisa menggunakan .then() setelah ini selesai
  return fetch("/getConfig") // Mengembalikan Promise dari fetch
    .then((r) => {
      if (!r.ok) {
        throw new Error("Failed to fetch config: " + r.statusText);
      }
      return r.json();
    })
    .then((data) => {
      document.getElementById("ntpServerSelect").value = data.ntpServer;
      document.getElementById("lastSyncTime").textContent =
        data.lastSyncTime || "Never";

      // NEW: Muat nilai timeOffset ke input
      const timeOffsetInput = document.getElementById("timeOffsetInput");
      if (timeOffsetInput) {
        // Pastikan elemen input ada di HTML
        // data.timeOffset datang dalam detik dari ESP32, konversi ke jam untuk UI
        timeOffsetInput.value = data.timeOffset / 3600 || 7; // Default 7 jam jika tidak ada
      }

      // NEW: Muat status Holiday Mode
      updateHolidayModeUI(data.holidayModeActive); // Panggil dengan data langsung

      for (let i = 0; i < 8; i++) {
        const tbody = document.getElementById(`config-${i}`);
        tbody.innerHTML = "";
        // Pastikan data.channels[i] ada sebelum mencoba iterasi
        // Gunakan optional chaining (?.) untuk menghindari error jika channels[i] undefined
        data.channels[i]?.forEach((config) => {
          // Pastikan 'config' sekarang sudah memiliki properti activeDays dari C++
          // Jika config.activeDays undefined, addTimerRow akan menggunakan default [true, true, ...]
          addTimerRow(i, config);
        });
      }
      console.log("Config loaded from ESP32.");
    })
    .catch((e) => {
      console.error("Error loading config:", e);
      showNotification("Error loading config: " + e.message, true);
    });
}

// WiFi Modal Functions
function openWifiModal() {
  document.getElementById("wifiModal").style.display = "block";
  scanNetworks();
}

function closeWifiModal() {
  document.getElementById("wifiModal").style.display = "none";
}

function scanNetworks() {
  const container = document.getElementById("wifiNetworks");
  container.innerHTML = "<p>Scanning networks...</p>"; // Tampilkan pesan scanning

  fetch("/scanWifi")
    .then((r) => r.json())
    .then((networks) => {
      container.innerHTML = ""; // Hapus pesan scanning

      if (networks.length === 0) {
        container.innerHTML = "<p>No networks found</p>";
        return;
      }

      networks.forEach((network) => {
        const div = document.createElement("div");
        div.style.padding = "8px";
        div.style.borderBottom = "1px solid #eee";
        div.style.cursor = "pointer";
        div.innerHTML = `
          <strong>${network.ssid}</strong> 
          <span style="float: right;">${network.rssi} dBm</span>
        `;
        div.addEventListener("click", function () {
          document.querySelectorAll("#wifiNetworks > div").forEach((el) => {
            el.style.backgroundColor = "";
          });
          this.style.backgroundColor = "#e0e0ff";
          document.getElementById("wifiPassword").focus();
        });
        container.appendChild(div);
      });
    })
    .catch((e) => {
      container.innerHTML =
        '<p style="color: red;">Error scanning networks.</p>';
      console.error("Error scanning WiFi:", e);
    });
}

function connectToWifi() {
  const selected = document.querySelector(
    '#wifiNetworks > div[style*="background-color"]'
  );
  if (!selected) {
    showNotification("Please select a network first", true);
    return;
  }

  const ssid = selected.querySelector("strong").textContent;
  const password = document.getElementById("wifiPassword").value;

  fetch("/setWifi", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(
      password
    )}`,
  })
    .then((r) => r.text())
    .then((msg) => {
      showNotification(msg);
      closeWifiModal();
      setTimeout(() => {
        window.location.reload(); // Reload halaman setelah koneksi
      }, 2000);
    })
    .catch((e) => showNotification("Connection failed: " + e, true));
}

// Event handlers
document.addEventListener("DOMContentLoaded", () => {
  initTabs();

  // Panggil loadConfig() terlebih dahulu
  loadConfig()
    .then(() => {
      updateClock();
      updateChannelStatusUI();
    })
    .catch((error) => {
      console.error("Error during initial load:", error);
    });

  document.getElementById("saveBtn").addEventListener("click", saveConfig);
  document.getElementById("syncTimeBtn").addEventListener("click", syncTime);
  document
    .getElementById("wifiConfigBtn")
    .addEventListener("click", openWifiModal);
  document
    .getElementById("holidayModeBtn")
    .addEventListener("click", toggleHolidayMode);
  document.querySelector(".close").addEventListener("click", closeWifiModal);
  document
    .getElementById("connectWifiBtn")
    .addEventListener("click", connectToWifi);

  document.addEventListener("click", (e) => {
    if (e.target.classList.contains("add-btn")) {
      const channel = e.target.dataset.channel;
      addTimerRow(channel);
    }
    if (e.target.classList.contains("delete-btn")) {
      e.target.closest("tr").remove();
    }
    if (e.target == document.getElementById("wifiModal")) {
      closeWifiModal();
    }

    // Manual Control Buttons
    if (e.target.classList.contains("manual-control-btn")) {
      const channel = e.target.dataset.channel;
      console.log("Clicked manual control button for channel:", channel);
      if (typeof channel === "undefined" || channel === null) {
        console.error("Channel data-attribute is missing!");
        showNotification("Error: Channel data is missing for button.", true);
        return;
      }
      let url = "/setManualControl?channel=" + channel;

      if (e.target.classList.contains("on-btn")) {
        url += "&state=true";
      } else if (e.target.classList.contains("off-btn")) {
        url += "&state=false";
      } else if (e.target.classList.contains("override-on-btn")) {
        url += "&override_on=true";
      } else if (e.target.classList.contains("override-off-btn")) {
        url += "&override_off=true";
      }

      fetch(url, { method: "POST" })
        .then((r) => r.text())
        .then((msg) => {
          showNotification(msg);
        })
        .catch((e) =>
          showNotification("Error controlling channel: " + e, true)
        );
    }
  });
});

// NEW: Update saveConfig to include activeDays
function saveConfig() {
  let allInputsAreValid = true;

  // NEW: Cek semua input waktu di semua channel
  document.querySelectorAll(".time-input").forEach((input) => {
    if (!validateTimeInput(input)) {
      allInputsAreValid = false;
    }
  });

  if (!allInputsAreValid) {
    showNotification(
      "Ada kesalahan dalam format waktu. Mohon perbaiki sebelum menyimpan!",
      true
    );
    return; // Hentikan proses save jika ada input tidak valid
  }

  // NEW: Tambahkan console.log() ini
  console.log(
    "Value from timeOffsetInput:",
    document.getElementById("timeOffsetInput").value
  );

  const config = {
    ntpServer: document.getElementById("ntpServerSelect").value,
    lastSyncTime: document.getElementById("lastSyncTime").textContent,
    holidayModeActive: document
      .getElementById("holidayModeBtn")
      .textContent.includes("ON"),
    // NEW: Tambahkan timeOffset ke objek config. Konversi jam ke detik.
    // Gunakan parseInt untuk memastikan nilainya angka, dan berikan default '7' jika input kosong.
    timeOffset:
      parseInt(document.getElementById("timeOffsetInput").value || "7") * 3600,
    channels: Array(8)
      .fill()
      .map((_, i) => {
        const rows = document.getElementById(`config-${i}`).rows;
        return Array.from(rows).map((row) => {
          const inputs = row.getElementsByTagName("input");
          const activeDays = [];
          row
            .querySelectorAll('.day-checkboxes input[type="checkbox"]')
            .forEach((cb) => {
              activeDays.push(cb.checked);
            });
          return {
            onTime: formatTimeInputValue(inputs[0]),
            offTime: formatTimeInputValue(inputs[1]),
            enable: inputs[2].checked,
            activeDays: activeDays,
          };
        });
      }),
  };

  // NEW: Tambahkan console.log() ini untuk melihat objek 'config' yang akan dikirim
  console.log("Config object to be sent:", config);
  console.log(
    "timeOffset in config object (before encoding):",
    config.timeOffset
  );

  fetch("/saveConfig", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `data=${encodeURIComponent(JSON.stringify(config))}`,
  })
    .then((r) => r.text())
    .then((msg) => {
      showNotification(msg);
      // Panggil loadConfig() untuk memperbarui tampilan semua pengaturan,
      // termasuk lastSyncTime dan nilai timeOffset di input.
      // Kemudian panggil updateClock() untuk memastikan waktu terkini juga diperbarui.
      loadConfig().then(() => {
        updateClock();
        console.log("UI updated after config save with new timezone.");
      });
    })
    .catch((e) => {
      console.error("Error saving config:", e);
      showNotification("Error saving config: " + e.message, true);
    });
} // Akhir dari saveConfig

function syncTime() {
  const btn = document.getElementById("syncTimeBtn");
  btn.disabled = true;

  fetch(
    "/syncTime?server=" +
      encodeURIComponent(document.getElementById("ntpServerSelect").value)
  )
    .then((r) => r.text())
    .then((msg) => {
      showNotification(msg);
      loadConfig(); // Reload config to update lastSyncTime
    })
    .catch((e) => showNotification("Error: " + e, true))
    .finally(() => (btn.disabled = false));
}

function showNotification(msg, isError = false) {
  const el = document.getElementById("notification");
  el.textContent = msg;
  el.className = isError ? "notification error" : "notification";
  el.style.display = "block";
  setTimeout(() => (el.style.display = "none"), 3000);
}

// Update clock every second
function updateClock() {
  fetch("/getTime")
    .then((r) => {
      if (!r.ok) {
        throw new Error(
          "Network response from /getTime was not ok: " + r.statusText
        );
      }
      return r.json();
    })
    .then((data) => {
      // NEW: Tambahkan console.log() ini untuk melihat data yang diterima
      console.log("Data received from /getTime:", data);
      console.log("Current Time from data:", data.currentTime);
      console.log("Last Sync Time from data:", data.lastSyncTime);

      // NEW: Tambahkan pengecekan null untuk elemen HTML
      const currentTimeEl = document.getElementById("currentTime");
      const lastSyncTimeEl = document.getElementById("lastSyncTime");

      if (currentTimeEl && data.currentTime) {
        currentTimeEl.textContent = data.currentTime;
      } else {
        console.error(
          "Error: 'currentTime' element or data.currentTime not found.",
          { currentTimeEl, data_currentTime: data.currentTime }
        );
      }

      if (lastSyncTimeEl && data.lastSyncTime) {
        lastSyncTimeEl.textContent = data.lastSyncTime || "Never";
      } else {
        console.error(
          "Error: 'lastSyncTime' element or data.lastSyncTime not found.",
          { lastSyncTimeEl, data_lastSyncTime: data.lastSyncTime }
        );
      }

      setTimeout(updateClock, 1000);
    })
    .catch((e) => {
      console.error("Error fetching time or parsing JSON from /getTime:", e);
      setTimeout(updateClock, 5000);
    });
}

// Dapatkan dan tampilkan status channel saat ini (NEW)
function updateChannelStatusUI() {
  fetch("/getChannelStatus")
    .then((r) => r.json())
    .then((data) => {
      data.forEach((chStatus, index) => {
        const statusEl = document.getElementById(`status-ch${index}`);
        const onBtn = document.querySelector(`#tab-${index} .on-btn`);
        const offBtn = document.querySelector(`#tab-${index} .off-btn`);
        const overrideOnBtn = document.querySelector(
          `#tab-${index} .override-on-btn`
        );
        const overrideOffBtn = document.querySelector(
          `#tab-${index} .override-off-btn`
        );

        if (statusEl && onBtn && offBtn && overrideOnBtn && overrideOffBtn) {
          statusEl.textContent = chStatus.state ? "ON" : "OFF";
          statusEl.className =
            "status-indicator " + (chStatus.state ? "on" : "off");

          if (chStatus.manualOverride) {
            statusEl.classList.add("override");
            statusEl.setAttribute("data-tooltip", "Manual Override Active");
            statusEl.textContent += " (Manual)";
            onBtn.disabled = false;
            offBtn.disabled = false;
            overrideOnBtn.disabled = true;
            overrideOffBtn.disabled = false;
          } else {
            statusEl.classList.remove("override");
            statusEl.removeAttribute("data-tooltip");
            onBtn.disabled = true;
            offBtn.disabled = true;
            overrideOnBtn.disabled = false;
            overrideOffBtn.disabled = true;
          }
        }
      });
      setTimeout(updateChannelStatusUI, 2000);
    })
    .catch((e) => {
      console.error("Error fetching channel status:", e);
      setTimeout(updateChannelStatusUI, 5000);
    });
}

// NEW: Fungsi untuk mengaktifkan/menonaktifkan Holiday Mode
function toggleHolidayMode() {
  const btn = document.getElementById("holidayModeBtn");
  const isCurrentlyActive = btn.classList.contains("active");
  const newState = !isCurrentlyActive;

  btn.disabled = true;

  console.log("Attempting to set Holiday Mode to:", newState);

  fetch("/setHolidayMode", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `active=${newState ? "true" : "false"}`,
  })
    .then((response) => {
      if (!response.ok) {
        throw new Error("Network response was not ok: " + response.statusText);
      }
      return response.text();
    })
    .then((msg) => {
      showNotification(msg);
      updateHolidayModeUI(newState);
      console.log("Holiday Mode response:", msg);
    })
    .catch((e) => {
      showNotification("Error setting Holiday Mode: " + e.message, true);
      console.error("Fetch error:", e);
    })
    .finally(() => {
      btn.disabled = false;
    });
}

// NEW: Fungsi untuk memperbarui tampilan tombol Holiday Mode berdasarkan status
function updateHolidayModeUI(isActiveFromServer = null) {
  const btn = document.getElementById("holidayModeBtn");

  if (isActiveFromServer !== null) {
    if (isActiveFromServer) {
      btn.classList.add("active");
      btn.textContent = "Holiday: ON"; // TEKS BARU
      btn.setAttribute(
        "data-tooltip",
        "Timers are disabled. Click to deactivate Holiday Mode."
      );
    } else {
      btn.classList.remove("active");
      btn.textContent = "Holiday: OFF"; // TEKS BARU
      btn.setAttribute(
        "data-tooltip",
        "Timers are active. Click to activate Holiday Mode."
      );
    }
    console.log(
      "Holiday Mode UI updated based on provided status:",
      isActiveFromServer
    );
  } else {
    fetch("/getHolidayMode")
      .then((r) => r.json())
      .then((data) => {
        if (data.active) {
          btn.classList.add("active");
          btn.textContent = "Holiday: ON";
          btn.setAttribute(
            "data-tooltip",
            "Timers are disabled. Click to deactivate Holiday Mode."
          );
        } else {
          btn.classList.remove("active");
          btn.textContent = "Holiday: OFF";
          btn.setAttribute(
            "data-tooltip",
            "Timers are active. Click to activate Holiday Mode."
          );
        }
        console.log(
          "Holiday Mode UI updated by fetching from server. Status:",
          data.active
        );
      })
      .catch((e) =>
        console.error("Error fetching Holiday Mode status for UI update:", e)
      );
  }
}

// NEW: Fungsi untuk validasi input waktu dan memberikan feedback visual
function validateTimeInput(inputElement) {
  const value = inputElement.value;
  // Regex untuk HH:MM:SS. Memastikan format 00-23, 00-59, 00-59
  const timeRegex = /^(0[0-9]|1[0-9]|2[0-3]):([0-5][0-9]):([0-5][0-9])$/;

  let isValid = true;
  let errorMessage = "";

  if (!timeRegex.test(value)) {
    isValid = false;
    errorMessage = "Format waktu harus HH:MM:SS (misal: 08:30:00).";
  } else {
    // Double check rentang numerik (regex sudah cukup ketat tapi ini lapisan pengamanan)
    const parts = value.split(":").map(Number);
    if (parts[0] > 23 || parts[1] > 59 || parts[2] > 59) {
      isValid = false; // Ini seharusnya sudah ditangkap regex, tapi jaga-jaga
      errorMessage = "Waktu tidak valid (HH:0-23, MM:0-59, SS:0-59).";
    }
  }

  if (!isValid) {
    inputElement.style.borderColor = "red"; // Border merah jika tidak valid
    inputElement.title = errorMessage; // Tooltip pesan error
  } else {
    inputElement.style.borderColor = ""; // Kembalikan border normal
    inputElement.title = ""; // Hapus tooltip
  }
  return isValid;
}

//</script>
